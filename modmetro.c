/**
	@file
	modmetro~: a modulating Max/MSP metronome for OS X
    based on simplemsp,
	original by: jeremy bernstein, jeremy@bootsquad.com,
    samm~,
    by Eric Lyon,
    and filebyte,
    from the Max 6.1 API examples.
	@ingroup examples	
*/

#include "ext.h"			// standard Max include, always required (except in Jitter)
#include "ext_obex.h"		// required for "new" style objects
#include "z_dsp.h"			// required for MSP objects


// struct to represent the object's state
typedef struct _modmetro {
	t_pxobject		ob;			// the object itself (t_pxobject in MSP instead of t_object)
	double			offset; 	// the value of a property of our object
    
    int bp_pos; /* pos in breakpoint array */
    int bp_length; /* number of breakpoints in array */
    double *breakpoints; /* heap array for each tempo factor */
    
    float sr; /* current sampling rate */
    double tempo; /* current master tempo */
    double samps; /* number of samples per beat */
    double metro; /* current metro count */
    
    short pause;
    short mute;
    short audiomod;
} t_modmetro;

t_symbol *ps_nothing;

// method prototypes
void *modmetro_new(t_symbol *s, long argc, t_atom *argv);
void modmetro_free(t_modmetro *x);
void modmetro_assist(t_modmetro *x, void *b, long m, long a, char *s);
void modmetro_float(t_modmetro *x, double f);
void modmetro_ft1(t_modmetro *x, double f);
void modmetro_dsp(t_modmetro *x, t_signal **sp, short *count);
void modmetro_dsp64(t_modmetro *x, t_object *dsp64, short *count, double samplerate, long maxvectorsize, long flags);
t_int *modmetro_perform(t_int *w);
void modmetro_perform64(t_modmetro *x, t_object *dsp64, double **ins, long numins, double **outs, long numouts, long sampleframes, long flags, void *userparam);

void modmetro_open(t_modmetro *x, t_symbol *s);
void modmetro_doopen(t_modmetro *x, t_symbol *s);

void modmetro_mute(t_modmetro *x, t_float f);
void modmetro_pause(t_modmetro *x);
void modmetro_arm(t_modmetro *x);
void modmetro_resume(t_modmetro *x);

void modmetro_audiomod(t_modmetro *x);

// global class pointer variable
static t_class *modmetro_class = NULL;


//***********************************************************************************************

int C74_EXPORT main(void)
{	
	// object initialization, note the use of dsp_free for the freemethod, which is required
	// unless you need to free allocated memory, in which case you should call dsp_free from
	// your custom free function.

	t_class *c = class_new("modmetro~", (method)modmetro_new, (method)modmetro_free, (long)sizeof(t_modmetro), 0L, A_GIMME, 0);
    
	class_addmethod(c, (method)modmetro_float,		"float",	A_FLOAT, 0);
    class_addmethod(c, (method)modmetro_ft1,		"ft1",      A_FLOAT, 0);    // for setting tempo
	class_addmethod(c, (method)modmetro_dsp,		"dsp",		A_CANT, 0);		// Old 32-bit MSP dsp chain compilation for Max 5 and earlier
	class_addmethod(c, (method)modmetro_dsp64,		"dsp64",	A_CANT, 0);		// New 64-bit MSP dsp chain compilation for Max 6
	class_addmethod(c, (method)modmetro_assist,	"assist",	A_CANT, 0);
	
    class_addmethod(c, (method)modmetro_open,      "open",		A_DEFSYM,0);
    
    class_addmethod(c, (method)modmetro_pause,     "pause",        0);
    class_addmethod(c, (method)modmetro_arm,       "arm",          0);
    class_addmethod(c, (method)modmetro_resume,    "resume",       0);
    class_addmethod(c, (method)modmetro_mute,      "mute",     A_FLOAT,0);
    
    class_addmethod(c, (method)modmetro_audiomod,  "audiomod",        0);
    
	class_dspinit(c);
	class_register(CLASS_BOX, c);
	modmetro_class = c;
    
    ps_nothing = gensym("");

	return 0;
}


void *modmetro_new(t_symbol *s, long argc, t_atom *argv)
{
	t_modmetro *x = (t_modmetro *)object_alloc(modmetro_class);

	if (x) {
		dsp_setup((t_pxobject *)x, 1);	// MSP inlets: arg is # of inlets and is REQUIRED!
										// use 0 if you don't need inlets
		outlet_new(x, "signal"); 		// signal outlet (note "signal" rather than NULL)
        inlet_new(x, NULL);
        floatin(x, 1);
		x->offset = 0.0;
	}
    
    /* get system sample rate */
    x->sr = sys_getsr();
    
    x->tempo = 0.0;
    
    if (argc > 0) {
        x->tempo = (double) atom_getfloatarg(0, argc, argv);
    }
    
    if (x->tempo <= 0.0) {
        x->tempo = 120.0;
        post("tempo autoset to 120 BPM");
    }
    
    x->samps = (60.0/x->tempo) * x->sr;
    post("beat length in samples: %f", x->samps);
    x->metro = 1.0;
    x->bp_pos = 0;
    x->bp_length = 0;
    x->pause = 0;
    x->mute = 0;
    x->audiomod = 0;
    
	return (x);
}

void modmetro_free(t_modmetro *x)
{
	dsp_free((t_pxobject *)x);
    free(x->breakpoints);
}

void modmetro_open(t_modmetro *x, t_symbol *s)
{
    defer_low(x,(method)modmetro_doopen,s,0,0L);
}

void modmetro_doopen(t_modmetro *x, t_symbol *s)
{
    short		path;
    char		ps[MAX_PATH_CHARS];
    char        finalpath[MAX_PATH_CHARS];
    t_fourcc	type;
    
    // filebyte_close(x);
    
    if (s==ps_nothing) {
        if (open_dialog(ps,&path,&type,0L,0))
            return;
    } else {
        strcpy(ps,s->s_name);
        if (locatefile_extended(ps,&path,&type,&type,-1)) {
            object_error((t_object *)x, "%s: can't find file",ps);
            return;
        }
    }
    
    path_toabsolutesystempath(path, ps, finalpath);
    post(finalpath);
    
    FILE *breaks;
    if ((breaks = fopen(finalpath, "r"))) {
        int counter = 0;
        char holder[20];
        char holder_b[20];
        while((fgets(holder, 20, breaks)) != NULL) {
            counter++;
        }
        post("%i breakpoints", counter);
        x->bp_length = counter;
        x->breakpoints = malloc(counter * sizeof(double));
        counter = 0;
        rewind(breaks);
        while((fgets(holder_b, 20, breaks)) != NULL) {
            x->breakpoints[counter] = atof(holder_b);
            post("bp: %f", x->breakpoints[counter]);
            counter++;
        }
        fclose(breaks);
    } else {
        post("file open failed!");
    }
}

void modmetro_ft1(t_modmetro *x, double f)
{
    double last_tempo;
    double tempo_fac;
    
    if (f <= 0.0) {
        error("illegal tempo: %f", f);
        return;
    }
    
    last_tempo = x->tempo;
    x->tempo = f;
    tempo_fac = last_tempo / x->tempo; // shrink or stretch factor for beats
    x->samps = (60.0/x->tempo) * x->sr;
    post("samps: %f", x->samps);
    x->metro *= tempo_fac;
}

void modmetro_pause(t_modmetro *x)
{
    x->pause = 1;
}

void modmetro_mute(t_modmetro *x, t_float f)
{
    x->mute = (short) f;
}

void modmetro_arm(t_modmetro *x)
{
    x->pause = 1;
    x->metro = 1.0;
}

void modmetro_resume(t_modmetro *x)
{
    x->pause = 0;
}

void modmetro_audiomod(t_modmetro *x)
{
    if (x->audiomod) {
        x->audiomod = 0;
    } else {
        x->audiomod = 1;
    }
}


//***********************************************************************************************

void modmetro_assist(t_modmetro *x, void *b, long m, long a, char *s)
{
	if (m == ASSIST_INLET) { //inlet
        switch (a) {
            case 0:
                sprintf(s, "(msg) Arm/start/pause, toggle signal multiplier, set tempo, etc.");
                break;
            case 1:
                sprintf(s, "(float) Set tempo");
                break;
            case 2:
                sprintf(s, "(signal) Tempo multiplier");
                break;
                
            default:
                break;
        }
	} 
	else {	// outlet
		sprintf(s, "(signal) el.samm~ style output for el.player~");
	}
}


void modmetro_float(t_modmetro *x, double f)
{
	x->offset = f;
}


//***********************************************************************************************

// this function is called when the DAC is enabled, and "registers" a function for the signal chain in Max 5 and earlier.
// In this case we register the 32-bit, "modmetro_perform" method.
void modmetro_dsp(t_modmetro *x, t_signal **sp, short *count)
{
	post("my sample rate is: %f", sp[0]->s_sr);
	
	// dsp_add
	// 1: (t_perfroutine p) perform method
	// 2: (long argc) number of args to your perform method
	// 3...: argc additional arguments, all must be sizeof(pointer) or long
	// these can be whatever, so you might want to include your object pointer in there
	// so that you have access to the info, if you need it.
	dsp_add(modmetro_perform, 4, x, sp[0]->s_vec, sp[1]->s_vec, sp[0]->s_n);
}


// this is the Max 6 version of the dsp method -- it registers a function for the signal chain in Max 6,
// which operates on 64-bit audio signals.
void modmetro_dsp64(t_modmetro *x, t_object *dsp64, short *count, double samplerate, long maxvectorsize, long flags)
{
	post("my sample rate is: %f", samplerate);
	
	// instead of calling dsp_add(), we send the "dsp_add64" message to the object representing the dsp chain
	// the arguments passed are:
	// 1: the dsp64 object passed-in by the calling function
	// 2: the symbol of the "dsp_add64" message we are sending
	// 3: a pointer to your object
	// 4: a pointer to your 64-bit perform method
	// 5: flags to alter how the signal chain handles your object -- just pass 0
	// 6: a generic pointer that you can use to pass any additional data to your perform method
	
	object_method(dsp64, gensym("dsp_add64"), x, modmetro_perform64, 0, NULL);
}


//***********************************************************************************************

// this is the 32-bit perform method for Max 5 and earlier
t_int *modmetro_perform(t_int *w)
{
	// DO NOT CALL post IN HERE, but you can call defer_low (not defer)
	
	// args are in a vector, sized as specified in modmetro_dsp method
	// w[0] contains &modmetro_perform, so we start at w[1]
	t_modmetro *x = (t_modmetro *)(w[1]);
	// t_float *inL = (t_float *)(w[2]);
	t_float *outL = (t_float *)(w[3]);
	int n = (int)w[4];
    short pause = x->pause;
	
	// this perform method simply copies the input to the output, offsetting the value
	/* while (n--)
		*outL++ = *inL++ + x->offset; */
    while (n--) {
        if (x->mute) {
            *outL = 0.0;
            return w + 5;
        }
        
        if (!pause) {
            x->metro -= 1.0;
        }
        
        if (x->metro <= 0) {
            *outL = 1.0;
            x->metro += x->samps;
            if (x->bp_pos < x->bp_length) {
                x->metro *= x->breakpoints[x->bp_pos];
            }
        } else {
            *outL = 0.0;
        }
    }
		
	// you have to return the NEXT pointer in the array OR MAX WILL CRASH
	return w + 5;
}


// this is 64-bit perform method for Max 6
void modmetro_perform64(t_modmetro *x, t_object *dsp64, double **ins, long numins, double **outs, long numouts, long sampleframes, long flags, void *userparam)
{
    t_double *outL = outs[0];
    t_double *inL = ins[0];
    short pause = x->pause;
    int n = sampleframes;
    
	/*
    t_double *inL = ins[0];		// we get audio for each inlet of the object from the **ins argument
	t_double *outL = outs[0];	// we get audio for each outlet of the object from the **outs argument
	int n = sampleframes;
	
	// this perform method simply copies the input to the output, offsetting the value
	while (n--)
		*outL++ = *inL++ + x->offset; */
    
    while (n--) {
        // if (!pause) {
            x->metro -= 1.0;
        // }
        
        if (x->metro <= 0) {
            if (x->mute) {
                *outL++ = 0.0;
            } else {
                *outL++ = 1.0;
            }
            x->metro += x->samps;
            if (x->audiomod) {
                x->metro *= (*inL++ + 1);
            }
            if (x->bp_pos < x->bp_length) {
                x->metro *= x->breakpoints[x->bp_pos];
                x->bp_pos++;
            }
        } else {
            *outL++ = 0.0;
        }
    }
}

