/*
** $Id: ldo.c,v 1.134 2001/04/11 18:39:37 roberto Exp roberto $
** Stack and Call structure of Lua
** See Copyright Notice in lua.h
*/


#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LUA_PRIVATE
#include "lua.h"

#include "ldebug.h"
#include "ldo.h"
#include "lgc.h"
#include "lmem.h"
#include "lobject.h"
#include "lparser.h"
#include "lstate.h"
#include "lstring.h"
#include "ltable.h"
#include "ltm.h"
#include "lundump.h"
#include "lvm.h"
#include "lzio.h"


/* space to handle stack overflow errors */
#define EXTRA_STACK   (2*LUA_MINSTACK)


static void restore_stack_limit (lua_State *L) {
  StkId limit = L->stack+(L->stacksize-EXTRA_STACK)-1;
  if (L->top < limit)
    L->stack_last = limit;
}


void luaD_init (lua_State *L, int stacksize) {
  stacksize += EXTRA_STACK;
  L->stack = luaM_newvector(L, stacksize, TObject);
  L->stacksize = stacksize;
  L->basefunc.base = L->top = L->stack;
  restore_stack_limit(L);
}


void luaD_stackerror (lua_State *L) {
  if (L->stack_last == L->stack+L->stacksize-1) {
    /* overflow while handling overflow */
    luaD_breakrun(L, LUA_ERRERR);  /* break run without error message */
  }
  else {
    L->stack_last += EXTRA_STACK;  /* to be used by error message */
    lua_assert(L->stack_last == L->stack+L->stacksize-1);
    luaD_error(L, l_s("stack overflow"));
  }
}


/*
** Adjust stack. Set top to base+extra, pushing NILs if needed.
** (we cannot add base+extra unless we are sure it fits in the stack;
**  otherwise the result of such operation on pointers is undefined)
*/
void luaD_adjusttop (lua_State *L, StkId base, int extra) {
  int diff = extra-(L->top-base);
  if (diff <= 0)
    L->top = base+extra;
  else {
    luaD_checkstack(L, diff);
    while (diff--)
      setnilvalue(L->top++);
  }
}


/*
** Open a hole inside the stack at `pos'
*/
static void luaD_openstack (lua_State *L, StkId pos) {
  int i = L->top-pos; 
  while (i--) setobj(pos+i+1, pos+i);
  incr_top;
}


static void dohook (lua_State *L, lua_Debug *ar, lua_Hook hook) {
  StkId old_top = L->top;
  luaD_checkstack(L, LUA_MINSTACK);  /* ensure minimum stack size */
  L->allowhooks = 0;  /* cannot call hooks inside a hook */
  lua_unlock(L);
  (*hook)(L, ar);
  lua_lock(L);
  lua_assert(L->allowhooks == 0);
  L->allowhooks = 1;
  L->top = old_top;
}


void luaD_lineHook (lua_State *L, int line, lua_Hook linehook) {
  if (L->allowhooks) {
    lua_Debug ar;
    ar.event = l_s("line");
    ar._ci = L->ci;
    ar.currentline = line;
    dohook(L, &ar, linehook);
  }
}


static void luaD_callHook (lua_State *L, lua_Hook callhook,
                           const l_char *event) {
  if (L->allowhooks) {
    lua_Debug ar;
    ar.event = event;
    ar._ci = L->ci;
    L->ci->pc = NULL;  /* function is not active */
    dohook(L, &ar, callhook);
  }
}


static StkId callCclosure (lua_State *L, const struct Closure *cl) {
  int nup = cl->nupvalues;  /* number of upvalues */
  int n;
  luaD_checkstack(L, nup+LUA_MINSTACK);  /* ensure minimum stack size */
  for (n=0; n<nup; n++)  /* copy upvalues as extra arguments */
    setobj(L->top++, &cl->upvalue[n]);
  lua_unlock(L);
  n = (*cl->f.c)(L);  /* do the actual call */
  lua_lock(L);
  return L->top - n;  /* return index of first result */
}


/*
** Call a function (C or Lua). The function to be called is at *func.
** The arguments are on the stack, right after the function.
** When returns, the results are on the stack, starting at the original
** function position.
** The number of results is nResults, unless nResults=LUA_MULTRET.
*/ 
void luaD_call (lua_State *L, StkId func, int nResults) {
  lua_Hook callhook;
  StkId firstResult;
  CallInfo ci;
  if (ttype(func) != LUA_TFUNCTION) {
    /* `func' is not a function; check the `function' tag method */
    Closure *tm = luaT_gettmbyObj(G(L), func, TM_FUNCTION);
    if (tm == NULL)
      luaG_typeerror(L, func, l_s("call"));
    luaD_openstack(L, func);
    setclvalue(func, tm);  /* tag method is the new function to be called */
  }
  ci.prev = L->ci;  /* chain new callinfo */
  L->ci = &ci;
  ci.base = func+1;
  callhook = L->callhook;
  if (callhook)
    luaD_callHook(L, callhook, l_s("call"));
  firstResult = (clvalue(func)->isC ? callCclosure(L, clvalue(func)) :
                                      luaV_execute(L, clvalue(func), func+1));
  if (callhook)  /* same hook that was active at entry */
    luaD_callHook(L, callhook, l_s("return"));
  L->ci = ci.prev;  /* unchain callinfo */
  /* move results to `func' (to erase parameters and function) */
  if (nResults == LUA_MULTRET) {
    while (firstResult < L->top)  /* copy all results */
      setobj(func++, firstResult++);
    L->top = func;
  }
  else {  /* copy at most `nResults' */
    for (; nResults > 0 && firstResult < L->top; nResults--)
      setobj(func++, firstResult++);
    L->top = func;
    for (; nResults > 0; nResults--) {  /* if there are not enough results */
      setnilvalue(L->top);  /* adjust the stack */
      incr_top;  /* must check stack space */
    }
  }
  luaC_checkGC(L);
}


/*
** Execute a protected call.
*/
struct CallS {  /* data to `f_call' */
  StkId func;
  int nresults;
};

static void f_call (lua_State *L, void *ud) {
  struct CallS *c = (struct CallS *)ud;
  luaD_call(L, c->func, c->nresults);
}


LUA_API int lua_call (lua_State *L, int nargs, int nresults) {
  StkId func;
  struct CallS c;
  int status;
  lua_lock(L);
  func = L->top - (nargs+1);  /* function to be called */
  c.func = func; c.nresults = nresults;
  status = luaD_runprotected(L, f_call, &c);
  if (status != 0)  /* an error occurred? */
    L->top = func;  /* remove parameters from the stack */
  lua_unlock(L);
  return status;
}


/*
** Execute a protected parser.
*/
struct SParser {  /* data to `f_parser' */
  ZIO *z;
  int bin;
};

static void f_parser (lua_State *L, void *ud) {
  struct SParser *p = (struct SParser *)ud;
  Proto *tf = p->bin ? luaU_undump(L, p->z) : luaY_parser(L, p->z);
  luaV_Lclosure(L, tf, 0);
}


static int protectedparser (lua_State *L, ZIO *z, int bin) {
  struct SParser p;
  lu_mem old_blocks;
  int status;
  lua_lock(L);
  p.z = z; p.bin = bin;
  /* before parsing, give a (good) chance to GC */
  if (G(L)->nblocks/8 >= G(L)->GCthreshold/10)
    luaC_collectgarbage(L);
  old_blocks = G(L)->nblocks;
  status = luaD_runprotected(L, f_parser, &p);
  if (status == 0) {
    /* add new memory to threshold (as it probably will stay) */
    lua_assert(G(L)->nblocks >= old_blocks);
    G(L)->GCthreshold += (G(L)->nblocks - old_blocks);
  }
  else if (status == LUA_ERRRUN)  /* an error occurred: correct error code */
    status = LUA_ERRSYNTAX;
  lua_unlock(L);
  return status;
}


LUA_API int lua_loadfile (lua_State *L, const l_char *filename) {
  ZIO z;
  int status;
  int bin;  /* flag for file mode */
  int nlevel;  /* level on the stack of filename */
  FILE *f = (filename == NULL) ? stdin : fopen(filename, l_s("r"));
  if (f == NULL) return LUA_ERRFILE;  /* unable to open file */
  bin = (ungetc(fgetc(f), f) == ID_CHUNK);
  if (bin && f != stdin) {
    fclose(f);
    f = fopen(filename, l_s("rb"));  /* reopen in binary mode */
    if (f == NULL) return LUA_ERRFILE;  /* unable to reopen file */
  }
  lua_pushliteral(L, l_s("@"));
  lua_pushstring(L, (filename == NULL) ? l_s("(stdin)") : filename);
  lua_concat(L, 2);
  nlevel = lua_gettop(L);
  filename = lua_tostring(L, -1);  /* filename = `@'..filename */
  luaZ_Fopen(&z, f, filename);
  status = protectedparser(L, &z, bin);
  lua_remove(L, nlevel);  /* remove filename */
  if (f != stdin)
    fclose(f);
  return status;
}


LUA_API int lua_loadbuffer (lua_State *L, const l_char *buff, size_t size,
                          const l_char *name) {
  ZIO z;
  int status;
  if (!name) name = l_s("?");
  luaZ_mopen(&z, buff, size, name);
  status = protectedparser(L, &z, buff[0]==ID_CHUNK);
  return status;
}



/*
** {======================================================
** Error-recover functions (based on long jumps)
** =======================================================
*/

/* chain list of long jump buffers */
struct lua_longjmp {
  jmp_buf b;
  struct lua_longjmp *previous;
  volatile int status;  /* error code */
};


static void message (lua_State *L, const l_char *s) {
  luaV_getglobal(L, luaS_newliteral(L, l_s(LUA_ERRORMESSAGE)), L->top);
  if (ttype(L->top) == LUA_TFUNCTION) {
    incr_top;
    setsvalue(L->top, luaS_new(L, s));
    incr_top;
    luaD_call(L, L->top-2, 0);
  }
}


/*
** Reports an error, and jumps up to the available recovery label
*/
void luaD_error (lua_State *L, const l_char *s) {
  if (s) message(L, s);
  luaD_breakrun(L, LUA_ERRRUN);
}


void luaD_breakrun (lua_State *L, int errcode) {
  if (L->errorJmp) {
    L->errorJmp->status = errcode;
    longjmp(L->errorJmp->b, 1);
  }
  else {
    if (errcode != LUA_ERRMEM)
      message(L, l_s("unable to recover; exiting\n"));
    exit(EXIT_FAILURE);
  }
}


int luaD_runprotected (lua_State *L, void (*f)(lua_State *, void *), void *ud) {
  CallInfo *oldci = L->ci;
  StkId oldtop = L->top;
  struct lua_longjmp lj;
  int allowhooks = L->allowhooks;
  lj.status = 0;
  lj.previous = L->errorJmp;  /* chain new error handler */
  L->errorJmp = &lj;
  if (setjmp(lj.b) == 0)
    (*f)(L, ud);
  else {  /* an error occurred: restore the state */
    L->allowhooks = allowhooks;
    L->ci = oldci;
    L->top = oldtop;
    restore_stack_limit(L);
  }
  L->errorJmp = lj.previous;  /* restore old error handler */
  return lj.status;
}

/* }====================================================== */

