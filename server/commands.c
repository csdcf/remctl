/*  $Id$
**
**  Running commands.
**
**  These are the functions for running external commands under remctld and
**  calling the appropriate protocol functions to deal with the output.
**
**  Written by Russ Allbery <rra@stanford.edu>
**  Based on work by Anton Ushakov
**  Copyright 2002, 2003, 2004, 2005, 2006
**      Board of Trustees, Leland Stanford Jr. University
**
**  See README for licensing terms.
*/

#include <config.h>
#include <system.h>

#include <errno.h>
#include <fcntl.h>
#include <sys/wait.h>

#include <server/internal.h>
#include <util/util.h>


/*
**  Processes the output from an external program.  Takes the client struct,
**  an array of file descriptors representing the output streams from the
**  client, and a count of streams.  Reads from all the streams as output is
**  available, stopping when they all reach EOF.
**
**  For protocol v2 and higher, we can send the output immediately as we get
**  it.  For protocol v1, we instead accumulate the output in the buffer
**  stored in our client struct, and will send it out later in conjunction
**  with the exit status.
**
**  Returns true on success, false on failure.
*/
static int
server_process_output(struct client *client, int fds[], int nfds)
{
    char junk[BUFSIZ];
    char *p;
    size_t left = MAXBUFFER;
    ssize_t *status;
    int i, maxfd, fd, result;
    fd_set fdset;

    /* If we haven't allocated an output buffer, do so now. */
    if (client->output == NULL)
        client->output = xmalloc(MAXBUFFER);
    p = client->output;

    /* Allocate the array of read statuses. */
    status = xmalloc(nfds * sizeof(ssize_t));
    for (i = 0; i < nfds; i++)
        status[i] = -1;

    /* Now, loop while we have input.  We no longer have input if the return
       status of read is 0 on all file descriptors.  At that point, we break
       out of the loop. */
    while (1) {
        FD_ZERO(&fdset);
        maxfd = -1;
        for (i = 0; i < nfds; i++) {
            if (status[i] != 0) {
                FD_SET(fds[i], &fdset);
                if (fds[i] > maxfd)
                    maxfd = fds[i];
            }
        }
        if (maxfd == -1)
            break;
        result = select(maxfd + 1, &fdset, NULL, NULL, NULL);
        if (result == 0)
            continue;
        if (result < 0) {
            syswarn("select failed");
            server_send_error(client, ERROR_INTERNAL, "Internal failure");
            free(status);
            return 0;
        }

        /* Iterate through each set file descriptor and read its output.   If
           we're using protocol version one, we append all the output together
           into the buffer.  Otherwise, we send an output token for each bit
           of output as we see it. */
        for (i = 0; i < nfds; i++) {
            fd = fds[i];
            if (!FD_ISSET(fd, &fdset))
                continue;
            if (client->protocol == 1) {
                if (left >= 0) {
                    status[i] = read(fd, p, left);
                    if (status[i] < 0 && (errno != EINTR && errno != EAGAIN))
                        goto readfail;
                    else if (status[i] > 0) {
                        p += status[i];
                        left -= status[i];
                    }
                } else {
                    status[i] = read(fd, junk, sizeof(junk));
                    if (status[i] < 0 && (errno != EINTR && errno != EAGAIN))
                        goto readfail;
                }
            } else {
                status[i] = read(fd, client->output, MAXBUFFER);
                if (status[i] < 0 && (errno != EINTR && errno != EAGAIN))
                    goto readfail;
                if (status[i] > 0) {
                    client->outlen = status[i];
                    if (!server_v2_send_output(client, i + 1))
                        goto fail;
                }
            }
        }
    }
    if (client->protocol == 1)
        client->outlen = p - client->output;
    return 1;

readfail:
    syswarn("read failed");
    server_send_error(client, ERROR_INTERNAL, "Internal failure");
fail:
    free(status);
    return 0;
}


/*
**  Process an incoming command.  Check the configuration files and the ACL
**  file, and if appropriate, forks off the command.  Takes the argument
**  vector and the user principal, and a buffer into which to put the output
**  from the executable or any error message.  Returns 0 on success and a
**  negative integer on failure.
**
**  Using the type and the service, the following argument, a lookup in the
**  conf data structure is done to find the command executable and acl file.
**  If the conf file, and subsequently the conf data structure contains an
**  entry for this type with service equal to "ALL", that is a wildcard match
**  for any given service.  The first argument is then replaced with the
**  actual program name to be executed.
**
**  After checking the acl permissions, the process forks and the child
**  execv's the command with pipes arranged to gather output. The parent waits
**  for the return code and gathers stdout and stderr pipes.
*/
void
server_run_command(struct client *client, struct config *config,
                   struct vector *argv)
{
    char *program;
    char *path = NULL;
    const char *type, *service, *user;
    struct confline *cline = NULL;
    int stdout_pipe[2], stderr_pipe[2], fds[2];
    char **req_argv = NULL;
    size_t i;
    int ret_code, ok;
    pid_t pid;

    /* We refer to these a lot, so give them good aliases. */
    type = argv->strings[0];
    service = argv->strings[1];
    user = client->user;

    /* Look up the command and the ACL file from the conf file structure in
       memory. */
    for (i = 0; i < config->count; i++) {
        cline = config->rules[i];
        if (strcmp(cline->type, type) == 0) {
            if (strcmp(cline->service, "ALL") == 0
                || strcmp(cline->service, service) == 0) {
                path = cline->program;
                break;
            }
        }
    }

    /* log after we look for command so we can get potentially get logmask */
    server_log_command(argv, path == NULL ? NULL : cline, user);

    /* Check the command, aclfile, and the authorization of this client to
       run this command. */
    if (path == NULL) {
        notice("unknown command %s %s from user %s", type, service, user);
        server_send_error(client, ERROR_UNKNOWN_COMMAND, "Unknown command");
        goto done;
    }
    if (!server_config_acl_permit(cline, user)) {
        notice("access denied: user %s, command %s %s", user, type, service);
        server_send_error(client, ERROR_ACCESS, "Access denied");
        goto done;
    }

    /* Assemble the argv, envp, and fork off the child to run the command. */
    req_argv = xmalloc((argv->count + 1) * sizeof(char *));

    /* Get the real program name, and use it as the first argument in argv
       passed to the command. */
    program = strrchr(path, '/');
    if (program == NULL)
        program = path;
    else
        program++;
    req_argv[0] = strdup(program);
    for (i = 1; i < argv->count; i++) {
        req_argv[i] = strdup(argv->strings[i]);
    }
    req_argv[i] = NULL;

    /* These pipes are used for communication with the child process that 
       actually runs the command. */
    if (pipe(stdout_pipe) != 0 || pipe(stderr_pipe) != 0) {
        syswarn("cannot create pipes");
        server_send_error(client, ERROR_INTERNAL, "Internal failure");
        goto done;
    }

    pid = fork();
    switch (pid) {
    case -1:
        syswarn("cannot fork");
        server_send_error(client, ERROR_INTERNAL, "Internal failure");
        goto done;

    /* In the child. */
    case 0:
        dup2(stdout_pipe[1], 1);
        close(stdout_pipe[0]);
        close(stdout_pipe[1]);
        dup2(stderr_pipe[1], 2);
        close(stderr_pipe[0]);
        close(stderr_pipe[1]);

        /* Child doesn't need stdin at all. */
        close(0);

        /* Put the authenticated principal in the environment.  SCPRINCIPAL is
           for backwards compatibility with sysctl. */
        if (setenv("REMUSER", client->user, 1) < 0) {
            syswarn("cannot set REMUSER in environment");
            exit(-1);
        }
        if (setenv("SCPRINCIPAL", client->user, 1) < 0) {
            syswarn("cannot set SCPRINCIPAL in environment");
            exit(-1);
        }

        /* Run the command. */
        execv(path, req_argv);

        /* This happens only if the exec fails.  Print out an error to pass up
           the stderr pipe; that's the best that we can do. */
        sysdie("exec failed");

    /* In the parent. */
    default:
        close(stdout_pipe[1]);
        close(stderr_pipe[1]);

        /* Unblock the read ends of the pipes, to enable us to read from both
           iteratevely. */
        fcntl(stdout_pipe[0], F_SETFL, 
              fcntl(stdout_pipe[0], F_GETFL) | O_NONBLOCK);
        fcntl(stderr_pipe[0], F_SETFL, 
              fcntl(stderr_pipe[0], F_GETFL) | O_NONBLOCK);

        /* This collects output from both pipes iteratively, while the child
           is executing, and processes it. */
        fds[0] = stdout_pipe[0];
        fds[1] = stderr_pipe[0];
        ok = server_process_output(client, fds, 2);
        waitpid(pid, &ret_code, 0);
        if (WIFEXITED(ret_code))
            ret_code = (signed int) WEXITSTATUS(ret_code);
        else
            ret_code = -1;
        if (ok) {
            if (client->protocol == 1)
                server_v1_send_output(client, ret_code);
            else
                server_v2_send_status(client, ret_code);
        }
    }

 done:
    if (req_argv != NULL) {
        i = 0;
        while (req_argv[i] != '\0') {
            free(req_argv[i]);
            i++;
        }
        free(req_argv);
    }
}