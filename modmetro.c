/**
	@file
	modmetro~ - modulating Max/MSP metronome for OS X
 
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
#include "ext_critical.h"	// critical regions shared with the audio thread
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


// struct to represent the object's state
typedef struct _modmetro {
	t_pxobject		ob;			// the object itself (t_pxobject in MSP instead of t_object)
	double			offset; 	// the value of a property of our object
    
    long bp_pos; /* pos in breakpoint array */
    long bp_length; /* number of breakpoints in array */
    double *breakpoints; /* heap array for each tempo factor */
    t_critical lock; /* guards the breakpoints and beat count, which perform64 uses on the audio thread */
    
    double sr; /* current sampling rate */
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
void modmetro_dsp64(t_modmetro *x, t_object *dsp64, short *count, double samplerate, long maxvectorsize, long flags);
void modmetro_perform64(t_modmetro *x, t_object *dsp64, double **ins, long numins, double **outs, long numouts, long sampleframes, long flags, void *userparam);

void modmetro_open(t_modmetro *x, t_symbol *s);
void modmetro_doopen(t_modmetro *x, t_symbol *s, short argc, t_atom *argv);

void modmetro_mute(t_modmetro *x, double f);
void modmetro_pause(t_modmetro *x);
void modmetro_arm(t_modmetro *x);
void modmetro_resume(t_modmetro *x);

void modmetro_audiomod(t_modmetro *x);

// global class pointer variable
static t_class *modmetro_class = NULL;


//***********************************************************************************************

void ext_main(void *r)
{
	// object initialization, note the use of dsp_free for the freemethod, which is required
	// unless you need to free allocated memory, in which case you should call dsp_free from
	// your custom free function.

	t_class *c = class_new("modmetro~", (method)modmetro_new, (method)modmetro_free, (long)sizeof(t_modmetro), 0L, A_GIMME, 0);
    
	class_addmethod(c, (method)modmetro_float,		"float",	A_FLOAT, 0);
    class_addmethod(c, (method)modmetro_ft1,		"ft1",      A_FLOAT, 0);    // for setting tempo
	class_addmethod(c, (method)modmetro_dsp64,		"dsp64",	A_CANT, 0);		// 64-bit MSP dsp chain compilation
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
}


void *modmetro_new(t_symbol *s, long argc, t_atom *argv)
{
	t_modmetro *x = (t_modmetro *)object_alloc(modmetro_class);

	if (!x) {
		return NULL;
	}

	dsp_setup((t_pxobject *)x, 1);	// MSP inlets: arg is # of inlets and is REQUIRED!
									// use 0 if you don't need inlets
	outlet_new(x, "signal"); 		// signal outlet (note "signal" rather than NULL)
    inlet_new(x, NULL);
    floatin(x, 1);
	x->offset = 0.0;
    
    critical_new(&x->lock);
    x->breakpoints = NULL;
    
    /* get system sample rate (dsp64 updates it when audio starts) */
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
    critical_free(x->lock);
}

void modmetro_open(t_modmetro *x, t_symbol *s)
{
    defer_low(x,(method)modmetro_doopen,s,0,0L);
}

void modmetro_doopen(t_modmetro *x, t_symbol *s, short argc, t_atom *argv)
{
    short		path;
    char		ps[MAX_PATH_CHARS];
    char        finalpath[MAX_PATH_CHARS];
    t_fourcc	type;
    FILE        *breaks;
    double      *newbps = NULL;
    double      *oldbps;
    double      val;
    long        count = 0;
    long        size = 0;
    
    if (s==ps_nothing) {
        if (open_dialog(ps,&path,&type,0L,0))
            return;
    } else {
        strncpy_zero(ps, s->s_name, MAX_PATH_CHARS);
        if (locatefile_extended(ps,&path,&type,&type,-1)) {
            object_error((t_object *)x, "%s: can't find file",ps);
            return;
        }
    }
    
    path_toabsolutesystempath(path, ps, finalpath);
    
    if (!(breaks = fopen(finalpath, "r"))) {
        object_error((t_object *)x, "couldn't open %s", finalpath);
        return;
    }
    /* one tempo factor per line; fscanf skips blank lines and a trailing newline */
    while (fscanf(breaks, "%lf", &val) == 1) {
        if (count == size) {
            double *grown;
            size = size ? size * 2 : 256;
            if (!(grown = realloc(newbps, size * sizeof(double)))) {
                object_error((t_object *)x, "out of memory reading %s", finalpath);
                free(newbps);
                fclose(breaks);
                return;
            }
            newbps = grown;
        }
        newbps[count++] = val;
    }
    if (!feof(breaks)) {
        object_warn((t_object *)x, "%s: stopped after %ld breakpoints at something that isn't a number", finalpath, count);
    }
    fclose(breaks);
    
    /* swap in the new breakpoints, starting from the first one, and free the old ones
       once perform64 can no longer be using them */
    critical_enter(x->lock);
    oldbps = x->breakpoints;
    x->breakpoints = newbps;
    x->bp_length = count;
    x->bp_pos = 0;
    critical_exit(x->lock);
    free(oldbps);
    
    object_post((t_object *)x, "read %ld breakpoints from %s", count, finalpath);
}

void modmetro_ft1(t_modmetro *x, double f)
{
    double last_tempo;
    double tempo_fac;
    
    if (f <= 0.0) {
        error("illegal tempo: %f", f);
        return;
    }
    
    critical_enter(x->lock);
    last_tempo = x->tempo;
    x->tempo = f;
    tempo_fac = last_tempo / x->tempo; // shrink or stretch factor for beats
    x->samps = (60.0/x->tempo) * x->sr;
    x->metro *= tempo_fac;
    critical_exit(x->lock);
}

void modmetro_pause(t_modmetro *x)
{
    x->pause = 1;
}

void modmetro_mute(t_modmetro *x, double f)
{
    x->mute = (short) f;
}

void modmetro_arm(t_modmetro *x)
{
    /* restart from the first beat and the first breakpoint. arm doesn't pause, so the
       clicks restart right away; send pause, arm, resume for a held start */
    critical_enter(x->lock);
    x->metro = 1.0;
    x->bp_pos = 0;
    critical_exit(x->lock);
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
                sprintf(s, "(signal) Tempo multiplier for audiomod, (msg) open, arm, pause, resume, mute, audiomod");
                break;
            case 1:
                sprintf(s, "(float) Set tempo");
                break;
            case 2:
                sprintf(s, "(msg) Same messages as the left inlet");
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

// registers a function for the signal chain, which operates on 64-bit audio signals
void modmetro_dsp64(t_modmetro *x, t_object *dsp64, short *count, double samplerate, long maxvectorsize, long flags)
{
    /* the beat length depends on the sample rate, which can differ from (or have changed
       since) the one modmetro_new got from sys_getsr() */
    critical_enter(x->lock);
    if (samplerate > 0.0 && samplerate != x->sr) {
        if (x->sr > 0.0) {
            x->metro *= samplerate / x->sr; /* keep the position within the current beat */
        }
        x->sr = samplerate;
        x->samps = (60.0/x->tempo) * x->sr;
    }
    critical_exit(x->lock);
	
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

// 64-bit perform method
void modmetro_perform64(t_modmetro *x, t_object *dsp64, double **ins, long numins, double **outs, long numouts, long sampleframes, long flags, void *userparam)
{
    t_double *outL = outs[0];
    t_double *inL = ins[0];
    short pause = x->pause;
    t_double in;
    long i;
    
    /* open, arm, tempo changes and dsp64 change these from other threads */
    critical_enter(x->lock);
    for (i = 0; i < sampleframes; i++) {
        in = inL[i]; /* read before writing outL[i]: MSP may process in place */
        
        if (!pause) {
            x->metro -= 1.0;
        }
        
        if (x->metro <= 0) {
            if (x->mute) {
                outL[i] = 0.0;
            } else {
                outL[i] = 1.0;
            }
            x->metro += x->samps;
            if (x->audiomod) {
                x->metro *= (in + 1);
            }
            if (x->bp_pos < x->bp_length) {
                x->metro *= x->breakpoints[x->bp_pos];
                x->bp_pos++;
            }
        } else {
            outL[i] = 0.0;
        }
    }
    critical_exit(x->lock);
}
