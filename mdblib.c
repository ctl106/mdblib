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
	char *cmnd[] = {MDB_EXEC, (char *)0};
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

	handle->buffer = malloc(size);
	vsnprintf(handle->buffer, size, format, arg);

printf("pdip_send():\t\"%s\"\n", handle->buffer);	// REMOVE_ME
	pdip_send(handle->pdip, handle->buffer);
	free(handle->buffer);
}

char *mdb_get(mdbhandle *handle)
{
	pdip_recv(handle->pdip, MDB_PROMPT_REG, handle->buffer, NULL, NULL, NULL);
printf("pdip_recv():\t\"%s\"\n", handle->buffer);	// REMOVE_ME

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
		mdb_put(handle, "break %s:%u %u", filename, linenumber, passCount);
	else
		mdb_put(handle, "break %s:%u", filename, linenumber);

	return mdb_bn_line(handle, filename, linenumber);
}

int mdb_break_addr(mdbhandle *handle, mdbptr address, unsigned int passCount)
{
	if (passCount)
		mdb_put(handle, "break *%"MDB_PRIXPTR" %u", address, passCount);
	else
		mdb_put(handle, "break *%"MDB_PRIXPTR, address);

	return mdb_bn_addr(handle, address);
}

int mdb_break_func(mdbhandle *handle, char *function, unsigned int passCount)
{
	if (passCount)
		mdb_put(handle, "break %s %u", function, passCount);
	else
		mdb_put(handle, "break %s", function);

	return mdb_bn_func(handle, function);
}

void mdb_delete(mdbhandle *handle, int breakpoint)
{
	mdb_put(handle, "delete %u", breakpoint);
}

void mdb_delete_all(mdbhandle *handle)
{
	mdb_put(handle, "delete");
}

int mdb_watch(mdbhandle *handle, mdbptr address, char *breakonType, unsigned int passCount)
{
	if (passCount)
		mdb_trans(handle, "watch %"MDB_PRIXPTR" %s %u", address, breakonType, passCount);
	else
		mdb_trans(handle, "watch %"MDB_PRIXPTR" %s", address, breakonType);

	return mdb_bn_addr(handle, address);
}


int mdb_watch_val(mdbhandle *handle, mdbptr address, char *breakonType, unsigned char value, size_t passCount)
{
	if (passCount)
		mdb_put(handle, "watch %"MDB_PRIXPTR" %s:%x %u", address, breakonType, value, passCount);
	else
		mdb_put(handle, "watch %"MDB_PRIXPTR" %s:%x", address, breakonType, value);

	return mdb_bn_addr(handle, address);
}


// data

long mdb_print_var(mdbhandle *handle, char f, size_t value, char *variable)
{
	char *result = NULL;
	if (value)
		result = mdb_trans(handle, "print /%c /datasize:%u %s", f, value, variable);
	else
		result = mdb_trans(handle, "print /%c %s", f, variable);
	return strtol(result, NULL, 0);
}

mdbptr mdb_print_var_addr(mdbhandle *handle, const char *variable)
{
	char *result = mdb_trans(handle, "print /a %s", variable);
	printf("\t\tmdb_print_var_addr() result:\t%s\n", result);	// REMOVE_ME
	mdbptr addr = 0;
	sscanf(result, "The Address of %s: 0x"MDB_SCNxPTR, NULL, addr);
	printf("\t\tpointer conversion:\t%"MDB_PRIXPTR"\n", addr);	// REMOVE ME
	return addr;
}

const char *mdb_print_pin(mdbhandle *handle, char *pinName)
{
	return mdb_trans(handle, "print pin %s", pinName);
}

void mdb_stim(mdbhandle *handle)
{
	mdb_put(handle, "stim");
}

void mdb_write_mem(mdbhandle *handle, char t, size_t addr, int wordc, mdbword wordv[])
{
	size_t size = wordc*(sizeof(mdbword) + 1);
	char *all_words = malloc(size);
	memset(all_words, 0, size);

	size_t i;
	for (i = 0; i < size; i++)
		sprintf(all_words, "%s%"MDB_PRIWORD" ", all_words, wordv[i]);
	all_words[size-1] = '\0';
	mdb_put(handle, "write /%c %x %s", t, addr, all_words);

	free(all_words);
}

void mdb_write_pins(mdbhandle *handle, char *pinName, int pinState)
{
	if (pinState)
		mdb_put(handle, "write %s high", pinName);
	else
		mdb_put(handle, "write %s low", pinName);
}

void mdb_write_pinv(mdbhandle *handle, char *pinName, int pinVoltage)
{
	mdb_put(handle, "write %s high", pinName, pinVoltage);
}

const char *mdb_x(mdbhandle *handle, char t, unsigned int n, char f, char u, mdbptr addr)
{
	mdb_put(handle, "x /%c%u%c%c %x", t, n, f, u, addr);
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
		mdb_trans(handle, "Hwtool %s -p %u", toolType, index);
	else
		mdb_trans(handle, "Hwtool %s %u", toolType, index);
}

char *mdb_hwtool_list(mdbhandle *handle)
{
	return mdb_trans(handle, "Hwtool");
}

// others
char *mdb_echo(mdbhandle *handle, char *text)
{
	return mdb_trans(handle, "echo %s", text);
}

char *mdb_help(mdbhandle *handle, char *text)
{
	char *result = NULL;
	if (text)
		result = mdb_trans(handle, "help %s", text);
	else
		result = mdb_trans(handle, "help");
	return result;
}

void mdb_quit(mdbhandle *handle)
{
	mdb_put(handle, "quit");
}

void mdb_set(mdbhandle *handle, char *tool_property_name, char *tool_property_value)
{
	mdb_put(handle, "set %s %s", tool_property_name, tool_property_value);
}

void mdb_sleep(mdbhandle *handle, unsigned int milliseconds)
{
	mdb_put(handle, "Sleep %u", milliseconds);
}

void mdb_stopwatch_val(mdbhandle *handle)
{
	mdb_put(handle, "Stopwatch");
}

void mdb_stopwatch_prop(mdbhandle *handle, char *stopwatch_property)
{
	mdb_put(handle, "Stopwatch %s", stopwatch_property);
}

void mdb_wait(mdbhandle *handle)
{
	mdb_put(handle, "Wait");
}

void mdb_wait_ms(mdbhandle *handle, unsigned int milliseconds)
{
	mdb_put(handle, "Wait %s", milliseconds);
}

void mdb_cd(mdbhandle *handle, char *DIR)
{
	mdb_put(handle, "cd %s", DIR);
}

mdbbp **mdb_info_break(mdbhandle *handle)
{
	mdb_trans(handle, "info breakpoints");
	return parse_breakpoints(handle->buffer);
}

mdbbp *mdb_info_break_n(mdbhandle *handle, size_t n)
{
	mdb_trans(handle, "info breakpoints %u", n);
	mdbbp **result = parse_breakpoints(handle->buffer);

	size_t i;
	for (i = 1; result[i] != NULL; i++)		// this should never be true, but
		mdb_close_breakpoint(result[i]);	// to be safe and avoid leaks

	return result[0];
}

char *mdb_list(mdbhandle *handle)
{
	return mdb_trans(handle, "list");
}

char *mdb_list_line(mdbhandle *handle, size_t linenum)
{
	return mdb_trans(handle, "list %u", linenum);
}

char *mdb_list_first(mdbhandle *handle, size_t first)
{
	return mdb_trans(handle, "list %u,", first);
}

char *mdb_list_last(mdbhandle *handle, size_t last)
{
	return mdb_trans(handle, "list ,%u", last);
}

char *mdb_list_ftol(mdbhandle *handle, size_t first, size_t last)
{
	return mdb_trans(handle, "list %u,%u", first, last);
}

char *mdb_list_prev(mdbhandle *handle)
{
	return mdb_trans(handle, "list -");
}

char *mdb_list_next(mdbhandle *handle)
{
	return mdb_trans(handle, "list +");
}

char *mdb_list_func(mdbhandle *handle, char *function)
{
	return mdb_trans(handle, "list %s", function);
}

char *mdb_list_fline(mdbhandle *handle, char *file, size_t linenum)
{
	return mdb_trans(handle, "list %s:%u", file, linenum);
}

char *mdb_list_ffunc(mdbhandle *handle, char *file, char *function)
{
	return mdb_trans(handle, "list %s:%s", file, function);
}

void mdb_set_list(mdbhandle *handle, size_t count)
{
	mdb_put(handle, "set system.listsize %u", count);
}

char *mdb_pwd(mdbhandle *handle)
{
	return mdb_trans(handle, "pwd");
}


// programming

void mdb_dump(mdbhandle *handle, char *m, char *filename)
{
	mdb_put(handle, "Dump -%s %s", m, filename);
}

void mdb_program(mdbhandle *handle, char *executableImageFile)
{
	mdb_put(handle, "Program %s", executableImageFile);
}

void mdb_upload(mdbhandle *handle)
{
	mdb_put(handle, "Upload");
}


// running

void mdb_continue(mdbhandle *handle)
{
	mdb_put(handle, "Continue");
}

void mdb_halt(mdbhandle *handle)
{
	mdb_put(handle, "halt");
}

void mdb_next(mdbhandle *handle)
{
	mdb_put(handle, "Next");
}

void mdb_run(mdbhandle *handle)
{
	mdb_put(handle, "Run");
}

void mdb_step(mdbhandle *handle)
{
	mdb_put(handle, "Step");
}

void mdb_stepi(mdbhandle *handle)
{
	mdb_put(handle, "Stepi %u");
}

void mdb_stepi_cnt(mdbhandle *handle, unsigned int count)
{
	mdb_put(handle, "Stepi %u", count);
}


// stack

char *mdb_backtrace(mdbhandle *handle, int full, int n)
{
	char *result = NULL;
	if (full)
		result = mdb_trans(handle, "backtrace full %d", n);
	else
		result = mdb_trans(handle, "backtrace %d", n);
	return result;
}

