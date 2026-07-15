#include "systemcalls.h"
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdlib.h>
#include <stdio.h>

/**
 * @param cmd the command to execute with system()
 * @return true if the command in @param cmd was executed
 *   successfully using the system() call, false if an error occurred,
 *   either in invocation of the system() call, or if a non-zero return
 *   value was returned by the command issued in @param cmd.
*/
bool do_system(const char *cmd)
{
    int status = system(cmd);

    if (status == -1)
    {
        // system() itself failed (e.g. fork()/waitpid() failed internally,
        // or a shell could not be executed)
        perror("do_system: system");
        return false;
    }

    if (WIFEXITED(status) && WEXITSTATUS(status) == 0)
    {
        return true;
    }

    return false;
}

/**
* @param count -The numbers of variables passed to the function. The variables are command to execute.
*   followed by arguments to pass to the command
*   Since exec() does not perform path expansion, the command to execute needs
*   to be an absolute path.
* @param ... - A list of 1 or more arguments after the @param count argument.
*   The first is always the full path to the command to execute with execv()
*   The remaining arguments are a list of arguments to pass to the command in execv()
* @return true if the command @param ... with arguments @param arguments were executed successfully
*   using the execv() call, false if an error occurred, either in invocation of the
*   fork, waitpid, or execv() command, or if a non-zero return value was returned
*   by the command issued in @param arguments with the specified arguments.
*/

bool do_exec(int count, ...)
{
    va_list args;
    va_start(args, count);
    char * command[count+1];
    int i;
    for(i=0; i<count; i++)
    {
        command[i] = va_arg(args, char *);
    }
    command[count] = NULL;

    // Flush stdout before fork() so any buffered output from the caller
    // isn't duplicated if the child process's exit path ever flushes
    // an inherited copy of the same buffer.
    fflush(stdout);

    pid_t pid = fork();

    if (pid == -1)
    {
        // fork() failed
        perror("do_exec: fork");
        va_end(args);
        return false;
    }
    else if (pid == 0)
    {
        // Child process: replace this process image with the requested command.
        // command[0] must be an absolute path since execv() does not search $PATH.
        execv(command[0], command);

        // execv() only returns if an error occurred
        perror("do_exec: execv");
        _exit(EXIT_FAILURE);
    }

    // Parent process: wait for the specific child to complete
    int status;
    if (waitpid(pid, &status, 0) == -1)
    {
        perror("do_exec: waitpid");
        va_end(args);
        return false;
    }

    va_end(args);

    return (WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

/**
* @param outputfile - The full path to the file to write with command output.
*   This file will be closed at completion of the function call.
* All other parameters, see do_exec above
*/
bool do_exec_redirect(const char *outputfile, int count, ...)
{
    va_list args;
    va_start(args, count);
    char * command[count+1];
    int i;
    for(i=0; i<count; i++)
    {
        command[i] = va_arg(args, char *);
    }
    command[count] = NULL;

    int fd = open(outputfile, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
    {
        perror("do_exec_redirect: open");
        va_end(args);
        return false;
    }

    // Flush stdout before fork(), same reasoning as in do_exec()
    fflush(stdout);

    pid_t pid = fork();

    if (pid == -1)
    {
        perror("do_exec_redirect: fork");
        close(fd);
        va_end(args);
        return false;
    }
    else if (pid == 0)
    {
        // Child process: point stdout at outputfile, then exec the command.
        if (dup2(fd, STDOUT_FILENO) < 0)
        {
            perror("do_exec_redirect: dup2");
            close(fd);
            _exit(EXIT_FAILURE);
        }
        close(fd);

        execv(command[0], command);

        // execv() only returns if an error occurred
        perror("do_exec_redirect: execv");
        _exit(EXIT_FAILURE);
    }

    // Parent process: this fd was only needed by the child, close our copy
    close(fd);

    int status;
    if (waitpid(pid, &status, 0) == -1)
    {
        perror("do_exec_redirect: waitpid");
        va_end(args);
        return false;
    }

    va_end(args);

    return (WIFEXITED(status) && WEXITSTATUS(status) == 0);
}