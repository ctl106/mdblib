#ifndef MDBLIB_H_INCLUDED
#define MDBLIB_H_INCLUDED


#define MDB_PRIXPTR	PRIXPTR
#define MDB_PRIxPTR	PRIxPTR
#define MDB_SCNxPTR	SCNxPTR
#define MDB_PRIWORD	"u"

typedef struct _mdbhandle	mdbhandle;
typedef uintptr_t			mdbptr;
typedef unsigned int		mdbword;

typedef enum _mdbstate {
	mdb_dead = 0,
	mdb_running,
	mdb_stopped,
	mdb_sleeping
} mdbstate;


/*	process management	*/
mdbhandle *mdb_init();		// launches an interactive mdb process
void mdb_close(mdbhandle *handle);	// makes sure the process closed

/*	basic I/O	*/
void mdb_put(mdbhandle *handle, const char *format, ...);
void mdb_vput(mdbhandle *handle, const char *format, va_list arg);
char *mdb_get(mdbhandle *handle);		// run after a put to collect output
char *mdb_trans(mdbhandle *handle, const char *format, ...);	// simple combo of the two

/*	utilities	*/
int mdb_bn_line(mdbhandle *handle, char *filename, size_t linenumber);
int mdb_bn_addr(mdbhandle *handle, mdbptr address);
int mdb_bn_func(mdbhandle *handle, char *function);
// mdb_search_breakpoints(mdbhandle *handle);

/*	mdb commands - implemented using mdb_put(mdbhandle *handle) and mdb_get(mdbhandle *handle)	*/
// breakpoints
int mdb_break_line(mdbhandle *handle, char *filename, size_t linenumber, size_t passCount);
int mdb_break_addr(mdbhandle *handle, mdbptr address, unsigned int passCount);
int mdb_break_func(mdbhandle *handle, char *function, unsigned int passCount);
void mdb_delete(mdbhandle *handle, int breakpoint);
void mdb_delete_all(mdbhandle *handle);
int mdb_watch(mdbhandle *handle, size_t address, char *breakonType, unsigned int passCount);
int mdb_watch_val(mdbhandle *handle, mdbptr address, char *breakonType, unsigned char value, size_t passCount);

// data
long mdb_print_var(mdbhandle *handle, char f, size_t value, char *variable);
mdbptr mdb_print_var_addr(mdbhandle *handle, char *variable);
const char *mdb_print_pin(mdbhandle *handle, char *pinName);
void mdb_stim(mdbhandle *handle);
void mdb_write_mem(mdbhandle *handle, char *t, size_t addr, mdbword words[]);
void mdb_write_pins(mdbhandle *handle, char *pinName, int pinState);
void mdb_write_pinv(mdbhandle *handle, char *pinName, int pinVoltage);
const char *mdb_x(mdbhandle *handle, char t, unsigned int n, char f, char u, size_t addr);

// deviceandtool
void mdb_device(mdbhandle *handle, char *devicename);
void mdb_hwtool(mdbhandle *handle, char *toolType, int p, size_t index);
char *mdb_hwtool_list(mdbhandle *handle);

// others
char *mdb_echo(mdbhandle *handle, char *text);
char *mdb_help(mdbhandle *handle, char *text);
void mdb_quit(mdbhandle *handle);
void mdb_set(mdbhandle *handle, char *tool_property_name, char *tool_property_value);
void mdb_sleep(mdbhandle *handle, unsigned int milliseconds);
void mdb_stopwatch_val(mdbhandle *handle);
void mdb_stopwatch_prop(mdbhandle *handle, char *stopwatch_property);
void mdb_wait(mdbhandle *handle);
void mdb_wait_ms(mdbhandle *handle, unsigned int milliseconds);
void mdb_cd(mdbhandle *handle, char *DIR);
char *mdb_info_break(mdbhandle *handle);
char *mdb_info_break_n(mdbhandle *handle, size_t n);
char *mdb_list(mdbhandle *handle);
char *mdb_list_line(mdbhandle *handle, size_t linenum);
char *mdb_list_first(mdbhandle *handle, size_t first);
char *mdb_list_last(mdbhandle *handle, size_t last);
char *mdb_list_ftol(mdbhandle *handle, size_t first, size_t last);
char *mdb_list_prev(mdbhandle *handle);
char *mdb_list_next(mdbhandle *handle);
char *mdb_list_func(mdbhandle *handle, char *function);
char *mdb_list_fline(mdbhandle *handle, char *file, size_t linenum);
char *mdb_list_ffunc(mdbhandle *handle, char *file, char *function);
void mdb_set_list(mdbhandle *handle, size_t count);
char *mdb_pwd(mdbhandle *handle);

// programming
void mdb_dump(mdbhandle *handle, char *m, char *filename);
void mdb_program(mdbhandle *handle, char *executableImageFile);
void mdb_upload(mdbhandle *handle);

// running
void mdb_continue(mdbhandle *handle);
void mdb_halt(mdbhandle *handle);
void mdb_next(mdbhandle *handle);
void mdb_run(mdbhandle *handle);
void mdb_step(mdbhandle *handle);
void mdb_stepi(mdbhandle *handle);
void mdb_stepi_cnt(mdbhandle *handle, unsigned int count);

// stack
char *mdb_backtrace(mdbhandle *handle, int full, int n);


#endif // MDBLIB_H_INCLUDED
