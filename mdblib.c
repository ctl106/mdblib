#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>

#include "pdip.h"

#include "mdblib.h"


#ifndef MDB_EXEC
#define MDB_EXEC "mdb"
#endif // MDB_EXEC

#ifndef MDB_PROMPT_REG
#define MDB_PROMPT_REG "^>"
#endif // MDB_PROMPT_REG


struct _mdbbp {
	int number;
	char enabled;
	mdbptr address;
	char *filename;
	size_t line;
};


struct _mdbhandle {
	pdip_cfg_t cfg;
	pdip_t pdip;
	int	pid;
	mdbstate state;
	char *buffer;
};


/*	utility functions	*/
mdbbp **parse_breakpoints(char *buffer)
{
	// expects buffer to be the output of mdb "info break"
	// first output line is just column lables, so ignore

	// given how this function is used in this library, tokenizing in-place
	// is perfectly acceptable, and preservation of buffer contents is unneeded
	enum {junk, number, enabled, address, filename, line};

	mdbbp **output;
	mdbbp *breakpoint;
	size_t o_size = 0;
	size_t f_size = 0;

	char *tok_buff = buffer;
	char *token;
	int type = junk;
	do {
		token = strtok(tok_buff, "\n\t ");

		if (strlen(token)) {
			switch (type) {
				case number:
					o_size++;	// not particularly efficient; optimize later
					output = realloc(output, o_size);
					output[o_size-1] = NULL;

					breakpoint = malloc(sizeof(breakpoint));
					breakpoint->number = atoi(token);
					type = enabled;
					break;
				case enabled:
					breakpoint->enabled = *token;
					type = address;
					break;
				case address:
					breakpoint->address = (mdbptr)atoi(token);
					type = filename;
					break;
				case filename:
					f_size = strlen(token);
					breakpoint->filename = malloc(f_size);
					strcpy(breakpoint->filename, token);
					type = line;
					break;
				case line:
					breakpoint->line = (size_t)atoi(token);
					type = number;

					output[o_size-1] = breakpoint;
					break;

				case junk:
				default:
					if (strcmp(token, "what") == 0)
						type = number;
			}
		}

		if (tok_buff)
			tok_buff = NULL;
	} while (token);

	if (output[o_size-1] == NULL)	// throw way incomplete breakpoint
		mdb_close_breakpoint(breakpoint);
	else {	// returned array MUST be null terminated
		output = realloc(output, o_size + 1);
		output[o_size] = NULL;
	}

	return output;
}

unsigned long long time_in_ms()
{
	// based off of the formula for calculating time in ms here:
	// https://stackoverflow.com/questions/16764276/measuring-time-in-millisecond-precision/16764286#16764286
	struct timeval tv;
	gettimeofday(&tv, NULL);
	return (unsigned long long)((tv.tv_sec * 1000) + (tv.tv_usec / 1000));
}


/*	process management	*/

mdbhandle *mdb_init()
{
	mdbhandle *handle = malloc(sizeof(mdbhandle));

	// set up pdip
	pdip_configure(1, 0);
	pdip_cfg_init(&(handle->cfg));
	handle->cfg.flags |= PDIP_FLAG_ERR_REDIRECT;
	handle->cfg.debug_level = 0;
	handle->pdip = pdip_new(&(handle->cfg));

	handle->state = mdb_stopped;
	handle->buffer = NULL;

	// technically using strlen() like this is hackish, but it should work
	// cmnd is a NULL terminated array.
	char *cmnd[2];
	cmnd[0] = MDB_EXEC;
	cmnd[1] = (char *)0;
	handle->pid = pdip_exec(handle->pdip, 1, cmnd);
printf("PID:\t%d\n", handle->pid);	// REMOVE_ME

	if (handle->pid < 1) {
		mdb_close(handle);
		handle = NULL;
	}

	mdb_get(handle);	// eat initial prompt

	return handle;
}

void mdb_close(mdbhandle *handle)
{
	int status = 0;
	pdip_status(handle->pdip, &status, 1);	// let the process exit gracefully
	pdip_delete(handle->pdip, NULL);

	handle->state = mdb_dead;
	free(handle->buffer);
	free(handle);
}


/*	basic I/O	*/

void mdb_put(mdbhandle *handle, const char *format, ...)
{
	va_list arg;
	va_start(arg, format);
	mdb_vput(handle, format, arg);
	va_end(arg);
}

void mdb_vput(mdbhandle *handle, const char *format, va_list arg)
{
	va_list arg2;
	va_copy(arg2, arg);
	size_t size = vsnprintf(NULL, 0, format, arg2) + 1;
	va_end(arg2);
	char *buffer = malloc(size);
	vsnprintf(buffer, size, format, arg);
printf("mdb_vput():\t\"%s\"\n", buffer);
	pdip_send(handle->pdip, buffer);
	free(buffer);
}

char *mdb_get(mdbhandle *handle)
{
	size_t basesize = 0;
	size_t datasize = 0;
	free(handle->buffer);
	handle->buffer = (char *)0;
	int result = pdip_recv(handle->pdip, MDB_PROMPT_REG, &handle->buffer, &basesize, &datasize, (struct timeval*)0);
printf("pdip_recv():\t%d\t%zd\t\"%s\"\n", result, datasize, handle->buffer);	// REMOVE_ME

	// check for breakpoint message
	static const char bp_msg[] = "Stop at";//"\nSingle breakpoint: @0x";
	static const char halted[] = "HALTED\n";
	//result = strncmp(handle->buffer, bp_msg, sizeof(bp_msg)/sizeof(char)-1);
	result = strstr(handle->buffer, bp_msg);
//printf("strncmp():\t%d\tsize:\t%d\n", result, sizeof(bp_msg)/sizeof(char)-1);
	if (result) {// == 0) {
printf("breakpoint message detected!\n");
		handle->state = mdb_stopped;
		// eat "HALTED" message
		pdip_recv(handle->pdip, halted, &handle->buffer, &basesize, &datasize, (struct timeval*)0);
		// read the actual message we were after
		result = pdip_recv(handle->pdip, MDB_PROMPT_REG, &handle->buffer, &basesize, &datasize, (struct timeval*)0);
printf("re-pdip_recv():\t%d\t%zd\t\"%s\"\n", result, datasize, handle->buffer);	// REMOVE_ME
	}

	return handle->buffer;
}

char *mdb_trans(mdbhandle *handle, const char *format, ...)
{
	va_list arg;
	va_start(arg, format);
	mdb_vput(handle, format, arg);
	va_end(arg);

	return mdb_get(handle);
}


/*	utilities	*/
int mdb_bn_line(mdbhandle *handle, char *filename, size_t linenumber)
{
	int number = -1;
	mdbbp **breakpoints = mdb_info_break(handle);

	size_t i;
	int loop;
	for (i = 0, loop = 1; loop; i++) {
		if (breakpoints[i] == NULL)
			loop = 0;
		else if (	// need something that handles file paths better
				strcmp(breakpoints[i]->filename, filename) == 0 &&
				breakpoints[i]->line == linenumber
				) {
			number = breakpoints[i]->number;
			loop = 0;
		}

		mdb_close_breakpoint(breakpoints[i]);
	}

	return number;
}

int mdb_bn_addr(mdbhandle *handle, mdbptr address)
{
	int number = -1;
	mdbbp **breakpoints = mdb_info_break(handle);

	size_t i;
	int loop;
	for (i = 0, loop = 1; loop; i++) {
		if (breakpoints[i] == NULL)
			loop = 0;
		else if (breakpoints[i]->address == address) {
			number = breakpoints[i]->number;
			loop = 0;
		}

		mdb_close_breakpoint(breakpoints[i]);
	}

	return number;
}

int mdb_bn_func(mdbhandle *handle, char *function)
{
	// tricky... it doesn't appear "info break" gives function names
	// it's kinda dumb that the commands for setting breakpoints
	// doesn't print out the breakpoint number.
#warning mdb_bn_func() does not return a useful value in current implementation
	return -1;
}

void mdb_close_breakpoint(mdbbp *breakpoint)
{
	free(breakpoint->filename);
	free(breakpoint);
}


/*	mdb commands	*/
// breakpoints

int mdb_break_line(mdbhandle *handle, char *filename, size_t linenumber, size_t passCount)
{
	if (passCount)
		mdb_trans(handle, "break %s:%u %u\n", filename, linenumber, passCount);
	else
		mdb_trans(handle, "break %s:%u\n", filename, linenumber);

	return mdb_bn_line(handle, filename, linenumber);
}

int mdb_break_addr(mdbhandle *handle, mdbptr address, unsigned int passCount)
{
	if (passCount)
		mdb_trans(handle, "break *%"MDB_PRIXPTR" %u\n", address, passCount);
	else
		mdb_trans(handle, "break *%"MDB_PRIXPTR"\n", address);

	return mdb_bn_addr(handle, address);
}

int mdb_break_func(mdbhandle *handle, char *function, unsigned int passCount)
{
	if (passCount)
		mdb_trans(handle, "break %s %u\n", function, passCount);
	else
		mdb_trans(handle, "break %s\n", function);

	return mdb_bn_func(handle, function);
}

void mdb_delete(mdbhandle *handle, int breakpoint)
{
	mdb_trans(handle, "delete %u\n", breakpoint);
}

void mdb_delete_all(mdbhandle *handle)
{
	mdb_trans(handle, "delete\n");
}

int mdb_watch(mdbhandle *handle, mdbptr address, char *breakonType, unsigned int passCount)
{
	char *result;
	size_t size;
	if (passCount) {
		result = mdb_trans(handle, "watch 0x%"MDB_PRIXPTR" %s %u\n", address, breakonType, passCount);
		size_t size = snprintf(NULL, 0, "watch 0x%"MDB_PRIXPTR" %s %u\nWatchpoint ", address, breakonType, passCount);
	}
	else {
		result = mdb_trans(handle, "watch 0x%"MDB_PRIXPTR" %s\n", address, breakonType);
		size_t size = snprintf(NULL, 0, "watch 0x%"MDB_PRIXPTR" %s\nWatchpoint ", address, breakonType);
	}

	int bn = strtol(result+size, NULL, 10);
	return bn;
}

int mdb_watch_val(mdbhandle *handle, mdbptr address, char *breakonType, unsigned char value, size_t passCount)
{
	if (passCount)
		mdb_trans(handle, "watch %"MDB_PRIXPTR" %s:%x %u\n", address, breakonType, value, passCount);
	else
		mdb_trans(handle, "watch %"MDB_PRIXPTR" %s:%x\n", address, breakonType, value);

	return mdb_bn_addr(handle, address);
}

int mdb_watch_name(mdbhandle *handle, const char *name, char *breakonType, unsigned int passCount)
{
	char *result;
	size_t size = 0;
	if (passCount) {
		result = mdb_trans(handle, "watch %s %s %u\n", name, breakonType, passCount);
		size = snprintf(NULL, 0, "watch %s %s %u\nWatchpoint ", name, breakonType, passCount);
	}
	else {
		result = mdb_trans(handle, "watch %s %s\n", name, breakonType);
		size = snprintf(NULL, 0, "watch %s %s\nWatchpoint ", name, breakonType);
	}
	int bn = strtol(result+size, NULL, 10);
	return bn;
}


int mdb_watch_name_val(mdbhandle *handle, const char *name, char *breakonType, unsigned char value, size_t passCount)
{
	if (passCount)
		mdb_trans(handle, "watch %s %s:%x %u\n", name, breakonType, value, passCount);
	else
		mdb_trans(handle, "watch %s %s:%x\n", name, breakonType, value);

	return 0;//mdb_bn_addr(handle, address);
}


// data

long mdb_print_var(mdbhandle *handle, char f, size_t value, char *variable)
{
	char *result = NULL;
	size_t size = 0;
	if (value) {
		result = mdb_trans(handle, "print /%c /datasize:%u %s\n", f, value, variable);
		if (f == 'a')
			size = snprintf(NULL, 0, "print /a /datasize:%u %s\nThe Address of %s: ", value, variable, variable);
		else
			size = snprintf(NULL, 0, "print /%c /datasize:%u %s\n%s=\n", f, value, variable, variable);
	} else {
		result = mdb_trans(handle, "print /%c %s\n", f, variable);
		if (f == 'a')
			size = snprintf(NULL, 0, "print /a %s\nThe Address of %s: ", variable, variable);
		else
			size = snprintf(NULL, 0, "print /%c %s\n%s=\n", f, variable, variable);
	}

	long out = strtol(result+size, NULL, 0);
printf("variable value:\t%ld\n", out);
	return out;
}

mdbptr mdb_print_var_addr(mdbhandle *handle, const char *variable)
{
	char *result = mdb_trans(handle, "print /a %s\n", variable);

	// size == offset of desired value
	size_t size = snprintf(NULL, 0, "print /a %s\nThe Address of %s: ", variable, variable);
	mdbptr addr = (mdbptr)strtol(result+size, NULL, 0);
	return addr;
}

const char *mdb_print_pin(mdbhandle *handle, char *pinName)
{
	return mdb_trans(handle, "print pin %s\n", pinName);
}

void mdb_stim(mdbhandle *handle)
{
	mdb_trans(handle, "stim\n");
}

void mdb_write_mem(mdbhandle *handle, char t, size_t addr, int wordc, mdbword wordv[])
{
	size_t size = wordc*(sizeof(mdbword) + 1);
	char *all_words = malloc(size);
	memset(all_words, 0, size);
printf("size:\t%d\n", size);
	size_t i;
	for (i = 0; i < size; i++)
		snprintf(all_words, size, "%s%"MDB_PRIWORD" ", all_words, wordv[i]);
	all_words[size-1] = '\0';
	mdb_trans(handle, "write /%c %x %s\n", t, addr, all_words);
printf("call to free...\n");
	free(all_words);
}

void mdb_write_pins(mdbhandle *handle, char *pinName, int pinState)
{
	if (pinState)
		mdb_trans(handle, "write %s high\n", pinName);
	else
		mdb_trans(handle, "write %s low\n", pinName);
}

void mdb_write_pinv(mdbhandle *handle, char *pinName, int pinVoltage)
{
	mdb_trans(handle, "write %s high\n", pinName, pinVoltage);
}

const char *mdb_x(mdbhandle *handle, char t, unsigned int n, char f, char u, mdbptr addr)
{
	mdb_trans(handle, "x /%c%u%c%c %x\n", t, n, f, u, addr);
	return mdb_get(handle);
}


// deviceandtool

void mdb_device(mdbhandle *handle, char *devicename)
{
	mdb_trans(handle, "Device %s\n", devicename);
}

void mdb_hwtool(mdbhandle *handle, char *toolType, int p, size_t index)
{
	if (p)
		mdb_trans(handle, "Hwtool %s -p %u\n", toolType, index);
	else
		mdb_trans(handle, "Hwtool %s %u\n", toolType, index);
}

char *mdb_hwtool_list(mdbhandle *handle)
{
	return mdb_trans(handle, "Hwtool\n");
}

// others
char *mdb_echo(mdbhandle *handle, char *text)
{
	return mdb_trans(handle, "echo %s\n", text);
}

char *mdb_help(mdbhandle *handle, char *text)
{
	char *result = NULL;
	if (text)
		result = mdb_trans(handle, "help %s\n", text);
	else
		result = mdb_trans(handle, "help\n");
	return result;
}

void mdb_quit(mdbhandle *handle)
{
	mdb_trans(handle, "quit\n");
}

void mdb_set(mdbhandle *handle, char *tool_property_name, char *tool_property_value)
{
	mdb_trans(handle, "set %s %s\n", tool_property_name, tool_property_value);
}

void mdb_sleep(mdbhandle *handle, unsigned int milliseconds)
{
	mdb_trans(handle, "Sleep %u\n", milliseconds);
}

void mdb_stopwatch_val(mdbhandle *handle)
{
	mdb_trans(handle, "Stopwatch\n");
}

void mdb_stopwatch_prop(mdbhandle *handle, char *stopwatch_property)
{
	mdb_trans(handle, "Stopwatch %s\n", stopwatch_property);
}

void mdb_wait(mdbhandle *handle)
{
	mdb_trans(handle, "Wait\n");
}

void mdb_wait_ms(mdbhandle *handle, unsigned int milliseconds)
{
	mdb_trans(handle, "Wait %s\n", milliseconds);
}

void mdb_cd(mdbhandle *handle, char *DIR)
{
	mdb_trans(handle, "cd %s\n", DIR);
}

mdbbp **mdb_info_break(mdbhandle *handle)
{
	mdb_trans(handle, "info breakpoints\n");
	return parse_breakpoints(handle->buffer);
}

mdbbp *mdb_info_break_n(mdbhandle *handle, size_t n)
{
	mdb_trans(handle, "info breakpoints %u\n", n);
	mdbbp **result = parse_breakpoints(handle->buffer);

	size_t i;
	for (i = 1; result[i] != NULL; i++)		// this should never be true, but
		mdb_close_breakpoint(result[i]);	// to be safe and avoid leaks

	return result[0];
}

char *mdb_list(mdbhandle *handle)
{
	return mdb_trans(handle, "list\n");
}

char *mdb_list_line(mdbhandle *handle, size_t linenum)
{
	return mdb_trans(handle, "list %u\n", linenum);
}

char *mdb_list_first(mdbhandle *handle, size_t first)
{
	return mdb_trans(handle, "list %u,\n", first);
}

char *mdb_list_last(mdbhandle *handle, size_t last)
{
	return mdb_trans(handle, "list ,%u\n", last);
}

char *mdb_list_ftol(mdbhandle *handle, size_t first, size_t last)
{
	return mdb_trans(handle, "list %u,%u\n", first, last);
}

char *mdb_list_prev(mdbhandle *handle)
{
	return mdb_trans(handle, "list -\n");
}

char *mdb_list_next(mdbhandle *handle)
{
	return mdb_trans(handle, "list +\n");
}

char *mdb_list_func(mdbhandle *handle, char *function)
{
	return mdb_trans(handle, "list %s\n", function);
}

char *mdb_list_fline(mdbhandle *handle, char *file, size_t linenum)
{
	return mdb_trans(handle, "list %s:%u\n", file, linenum);
}

char *mdb_list_ffunc(mdbhandle *handle, char *file, char *function)
{
	return mdb_trans(handle, "list %s:%s\n", file, function);
}

void mdb_set_list(mdbhandle *handle, size_t count)
{
	mdb_trans(handle, "set system.listsize %u\n", count);
}

char *mdb_pwd(mdbhandle *handle)
{
	return mdb_trans(handle, "pwd\n");
}


// programming

void mdb_dump(mdbhandle *handle, char *m, char *filename)
{
	mdb_trans(handle, "Dump -%s %s\n\n", m, filename);
}

void mdb_program(mdbhandle *handle, char *executableImageFile)
{
	mdb_trans(handle, "Program %s\n", executableImageFile);
}

void mdb_upload(mdbhandle *handle)
{
	mdb_trans(handle, "Upload\n");
}


// running

void mdb_continue(mdbhandle *handle)
{
	mdb_trans(handle, "Continue\n");
	handle->state = mdb_running;
}

void mdb_halt(mdbhandle *handle)
{
	mdb_trans(handle, "halt\n");
}

void mdb_next(mdbhandle *handle)
{
	mdb_trans(handle, "Next\n");
}

void mdb_run(mdbhandle *handle)
{
	mdb_trans(handle, "Run\n");
}

void mdb_step(mdbhandle *handle)
{
	mdb_trans(handle, "Step\n");
}

void mdb_stepi(mdbhandle *handle)
{
	mdb_trans(handle, "Stepi\n");
}

void mdb_stepi_cnt(mdbhandle *handle, unsigned int count)
{
	mdb_trans(handle, "Stepi %u\n", count);
}


// stack

char *mdb_backtrace(mdbhandle *handle, int full, int n)
{
	char *result = NULL;
	if (full)
		result = mdb_trans(handle, "backtrace full %d\n", n);
	else
		result = mdb_trans(handle, "backtrace %d\n", n);
	return result;
}

