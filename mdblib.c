#include <errno.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

#include "mdblib.h"


#ifndef MDB_EXEC
#define MDB_EXEC "mdb"
#endif // MDB_EXEC


struct _mdbhandle{
	pid_t pid;
	mdbstate state;
	int ipipe;
	int opipe;
	char *buffer;
};


/*	utility functions	*/
void child_fork(int ipipe[2], int opipe[2])
{
	while (dup2(ipipe[0], STDIN_FILENO) < 0 && (errno == EINTR));
	while (dup2(opipe[1], STDOUT_FILENO) < 0 && (errno == EINTR));
	while (dup2(opipe[1], STDERR_FILENO) < 0 && (errno == EINTR));
	close(ipipe[0]);
	close(ipipe[1]);
	close(opipe[0]);
	close(opipe[1]);
	execlp(MDB_EXEC, MDB_EXEC, (char *)NULL);
}

void failed_fork(int ipipe[2], int opipe[2])
{
	close(ipipe[0]);
	close(ipipe[1]);
	close(opipe[0]);
	close(opipe[1]);
}

mdbhandle *parent_fork(pid_t pid, int ipipe[2], int opipe[2])
{
	close(ipipe[0]);
	close(opipe[1]);

	mdbhandle *handle = malloc(sizeof(mdbhandle));
	handle->pid = pid;
	handle->state = mdb_stopped;
	handle->ipipe = ipipe[1];
	handle->opipe = opipe[0];
	handle->buffer = NULL;
	return handle;
}


/*	process management	*/

mdbhandle *mdb_init()
{
	mdbhandle *handle = NULL;

	// setup pipes
	int ipipe[2];
	int opipe[2];
	pipe(ipipe);
	pipe(opipe);

	pid_t pid = fork();

	if (pid == 0)
		child_fork(ipipe, opipe);
	else if (pid > 0)
		handle = parent_fork(pid, ipipe, opipe);
	else
		failed_fork(ipipe, opipe);

	return handle;
}

void mdb_close(mdbhandle *handle)
{
	waitpid(handle->pid, NULL, 0);
	handle->state = mdb_dead;
	close(handle->opipe);
	close(handle->ipipe);
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

	free(handle->buffer);
	handle->buffer = malloc(size);
	vsnprintf(handle->buffer, size, format, arg);
	write(handle->ipipe, handle->buffer, size);
}

char *mdb_get(mdbhandle *handle)
{
	const size_t basesize = 100;
	size_t size = 0;
	size_t offset = 0;
	size_t bread = 0;
	free(handle->buffer);
	handle->buffer = NULL;

	do {
		offset += bread;
		if ((size-offset) < basesize) {
			size += basesize;
			handle->buffer = realloc(handle->buffer, size);
		}

		bread = read(handle->opipe, handle->buffer+offset, size-offset);
	} while (bread > 0);

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


/*	mdb commands - implemented using mdb_put(mdbhandle handle) and mdb_get(mdbhandle *handle)	*/
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
		mdb_put(handle, "watch %"MDB_PRIXPTR" %s %u", address, breakonType, passCount);
	else
		mdb_put(handle, "watch %"MDB_PRIXPTR" %s", address, breakonType);

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

mdbptr mdb_print_var_addr(mdbhandle *handle, char *variable)
{
	char *result = mdb_trans(handle, "print /a %s", variable);
	mdbptr addr = 0;
	sscanf(result, MDB_SCNxPTR, addr);
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

void mdb_write_mem(mdbhandle *handle, char *t, size_t addr, mdbword words[])
{
	char *all_words;
	/*	MAGIC	*/

	mdb_put(handle, "write /%c %x %s", t, addr, all_words);
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

const char *mdb_x(mdbhandle *handle, char t, unsigned int n, char f, char u, size_t addr)
{
	mdb_put(handle, "x /%c%u%c%c %x", t, n, f, u, addr);
	return mdb_get(handle);
}


// deviceandtool

void mdb_device(mdbhandle *handle, char *devicename)
{
	mdb_put(handle, "Device %s", devicename);
}

void mdb_hwtool(mdbhandle *handle, char *toolType, int p, size_t index)
{
	if (p)
		mdb_put(handle, "Hwtool %s -p %u", toolType, index);
	else
		mdb_put(handle, "Hwtool %s %u", toolType, index);
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

char *mdb_info_break(mdbhandle *handle)
{
	return mdb_trans(handle, "info breakpoints");
}

char *mdb_info_break_n(mdbhandle *handle, size_t n)
{
	return mdb_trans(handle, "info breakpoints %u", n);
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

