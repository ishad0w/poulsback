#ifndef GLUT_WRAP_H
#define GLUT_WRAP_H

#ifdef HAVE_FREEGLUT
#  include <GL/freeglut.h>
#elif defined __APPLE__
#  include <GLUT/glut.h>
#  define APIENTRY
#else
#  include <GL/glut.h>
#endif

#ifndef GLAPIENTRY
#ifdef _WIN32
#define GLAPIENTRY __stdcall
#else
#define GLAPIENTRY
#endif
#endif

#endif /* ! GLUT_WRAP_H */
