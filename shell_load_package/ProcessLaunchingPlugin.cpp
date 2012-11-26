/* Copyright (c) 2005 - 2012 Vertica, an HP company -*- C++ -*- */

#include "ProcessLaunchingPlugin.h"
#include "Vertica.h"
#include "LoadArgParsers.h"
#include "popen3.h"
#include <stdio.h>
#include <unistd.h>
#include <sys/wait.h>
#include <errno.h>
#include <fcntl.h>

using namespace Vertica;

ProcessLaunchingPlugin::ProcessLaunchingPlugin(std::string cmd, std::vector<std::string> env) : cmd(cmd), env(env) {
    error.size = 2048;
    error.buf = (char *)malloc(error.size);
    error.offset = 0;
}

#ifndef NO_SUDO
#define ProcessLaunchingPluginArgv(...) {"/usr/bin/sudo", "-E", "-u", "nobody", "-n", "--", __VA_ARGS__, NULL}
#else
#define ProcessLaunchingPluginArgv(...) {__VA_ARGS__, NULL}
#endif

void ProcessLaunchingPlugin::setupProcess() {
    // Convert std::vector<std::string> to char *const envp[]
    std::vector<const char *>cStrArray(env.size()+1, NULL);
    for(int i = 0; i < env.size(); i++) {
        cStrArray[i] = env[i].c_str();
    }
    
    // Open child
    char *const argv[] = ProcessLaunchingPluginArgv("/bin/sh", "-c", const_cast<char *const>(cmd.c_str()));
    child = popen3(argv[0], argv, const_cast<char **>(&cStrArray[0]), O_NONBLOCK);

    // Validate the file handle; make sure we can read from this file
    if (child.pid < 0) {
        vt_report_error(0, "Error running child process (cmd = %s, code = %d): %s", cmd.c_str(), errno, strerror(errno));
    }
}

// Pumps data
//   - from input to child process's stdin, and
//   - from child process's stdout to output.
StreamState ProcessLaunchingPlugin::pump(DataBuffer &input, InputState input_state, DataBuffer &output) {
    if (output.size == output.offset && input.size == input.offset && input_state != END_OF_FILE) {
        vt_report_error(0, "Can't read nor write, why poll?");
    }
    
    bool stdin_can_accept_data, stdout_has_data, stderr_has_data;
    if (ppoll3(child, &stdin_can_accept_data, &stdout_has_data, &stderr_has_data, 10)) {
        int err = errno;
        vt_report_error(0, "Error while doing poll() (%d): %s)", err, strerror(err));
    }
    
    // Buffer states
    bool input_buffer_empty = input.offset == input.size;
    bool output_buffer_full = output.offset == output.size;
    bool error_buffer_full  = error.offset == error.size;
    
    // If upstream is done, close our stdin handle
    // so that our wrapped process knows to stop.
    // Don't do this if we're currently processing data;
    // it's unnecessary and the received-data event
    // masks the EOF on some platforms with this implementation.
    if (input_state == END_OF_FILE && input_buffer_empty && child.stdin >= 0) {
        close(child.stdin);
        child.stdin = -1;
    }
    
    // Check stderr first since we treat it as fatal
    if (stderr_has_data && !error_buffer_full) {
        if (error.offset == 0) {
            // First error output, log the time.
            first_error_time = time(NULL);
        }
        ssize_t bytes_read = read(child.stderr, error.buf + error.offset, error.size - error.offset);
        if (bytes_read < 0) {
            int err = errno;
            if (err == EAGAIN || err == EWOULDBLOCK) {
                // Nothing to read, ignore
            } else {
                vt_report_error(0, "Error while reading stderr of external process (%d): %s", err, strerror(err));
            }
        } else if (bytes_read > 0) {
            error.offset += bytes_read;
        } else if (bytes_read == 0) {
            // Child process closed it's stderr
            close(child.stderr);
            child.stderr = -1;
        }
    }
    
    if (stdin_can_accept_data && !input_buffer_empty) {
        ssize_t bytes_written = write(child.stdin, input.buf + input.offset, input.size - input.offset);
        if (bytes_written < 0) {
            int err = errno;
            vt_report_error(0, "Error while writing to stdin of external process (%d): %s", err, strerror(err));
        } else if (bytes_written >= 0) {
            input.offset += bytes_written;
        }
    }
    
    if (stdout_has_data && !output_buffer_full) {
        ssize_t bytes_read = read(child.stdout, output.buf + output.offset, output.size - output.offset);
        if (bytes_read < 0) {
            int err = errno;
            if (err == EAGAIN || err == EWOULDBLOCK) {
                // Nothing to read, ignore
            } else {
                vt_report_error(0, "Error while reading stdout of external process (%d): %s", err, strerror(err));
            }
        } else if (bytes_read > 0) {
            output.offset += bytes_read;
        } else if (bytes_read == 0) {
            // Child process closed it's stdout
            close(child.stdout);
            child.stdout = -1;
        }
    }
    
    // Buffer states after reading and writing
    input_buffer_empty = input.offset == input.size;
    output_buffer_full = output.offset == output.size;
    error_buffer_full  = error.offset == error.size;
    
    // Return value
    if (error.offset > 0) {
        // Process has written to stderr => fail
        if (time(NULL) < first_error_time + 2 && child.stderr != -1) {
            // Give it at least 1 second, in case it did something like
            // write('Error: ') and the meaningful message is yet to come.
            return KEEP_GOING;
        } else {
            error.buf[min(error.offset, error.size-1)] = '\0';
            vt_report_error(0, "External process '%s' reported error: %s", cmd.c_str(), error.buf);
        }
    } else if (child.stdin == -1 && child.stdout == -1 && child.stderr == -1) {
        checkProcessStatus(child.pid);
        child.pid = -1;
        return DONE;
    } else {
        if (input_state != END_OF_FILE && input_buffer_empty && child.stdin >= 0) {
            return INPUT_NEEDED;
        } else if (output_buffer_full && child.stdout >= 0) {
            return OUTPUT_NEEDED;
        } else {
            return KEEP_GOING;
        }
    }
}

void ProcessLaunchingPlugin::destroyProcess() {
    pclose3(child);
    if (child.pid != -1) {
        // TODO: Kill child to avoid having to wait 1 hour if error was reported anyway.
        //       (it's a little problematic, since sudo runs as root and we don't have perms)
        waitpid(child.pid, NULL, 0);
    }
}

// Does a waitpid() and calls vt_report_error if process did not exit with 0.
void ProcessLaunchingPlugin::checkProcessStatus(pid_t pid) {
    int status;
    pid_t waitpid_ret = waitpid(pid, &status, 0);
    if (waitpid_ret == -1) {
        int err = errno;
        vt_report_error(0, "Error retrieving the termination status of child (%d): %s", err, strerror(err));
    } else if (waitpid_ret == pid) {
        // Child terminated, check termination status
        if (WIFEXITED(status)) {
            if (WEXITSTATUS(status) == 0) {
                // Success!
            } else {
                vt_report_error(0, "Process exited with status %d", WEXITSTATUS(status));
            }
        } else if (WIFSIGNALED(status)) {
            vt_report_error(0, "Process killed by signal %d%s", WTERMSIG(status), WCOREDUMP(status) ? " (core dumped)" : "");
        } else {
            vt_report_error(0, "Process terminated with unexpected status - 0x%x\n", status);
        }
    } else {
        vt_report_error(0, "Internal error: waitpid returned %d (child pid is %d)", (int)waitpid_ret, pid);
    }
}

ProcessLaunchingPlugin::~ProcessLaunchingPlugin() {
    free(error.buf);
}
