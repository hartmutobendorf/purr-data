#include "defines.h"

/*--------------- vrmstodb ----------------*/

static t_class *vrmstodb_class;

typedef struct _vrmstodb
{
    t_object x_obj;
} t_vrmstodb;

#ifndef __EMSCRIPTEN__
float rmstodb(float f)
{
    if (f <= 0) return (0);
    else
    {
    	float val = (float)(100 + 20./LOGTEN * log(f));
    	return (val < 0 ? 0 : val);
    }
}
#endif /* __EMSCRIPTEN__ */

static void vrmstodb_perform(t_vrmstodb *x, t_symbol *s, int argc, t_atom *argv)
{
	int i;
	t_atom *ap,*app;
    ap = (t_atom *)getbytes(sizeof(t_atom)*argc);
	app=ap;

	for (i = 0; i < argc; i++)
	{
		SETFLOAT(app, rmstodb(atom_getfloat(argv++)));
		app++;
	}
	outlet_list(x->x_obj.ob_outlet,gensym("list"),argc,ap);
    freebytes(ap,argc);
}

static void *vrmstodb_new()
{
	t_vrmstodb *x=(t_vrmstodb *)pd_new(vrmstodb_class);
	outlet_new(&x->x_obj, gensym("list"));
	return (void *)x;
}

void vrmstodb_setup(void)
{
    vrmstodb_class = class_new(gensym("vrmstodb"),
    	(t_newmethod)vrmstodb_new, 0,
		sizeof(t_vrmstodb), 
		CLASS_DEFAULT,
	    0);
    class_addlist(vrmstodb_class, (t_method)vrmstodb_perform);
}

