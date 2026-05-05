/* Minimal termcap.h for cross-compiling vim without ncurses. */
#ifndef _TERMCAP_H
#define _TERMCAP_H

extern int    tgetent(char *bp, const char *name);
extern int    tgetnum(const char *id);
extern int    tgetflag(const char *id);
extern char  *tgetstr(const char *id, char **area);
extern char  *tgoto(const char *cm, int col, int line);
extern int    tputs(const char *cp, int affcnt, int (*outc)(int));

#endif
