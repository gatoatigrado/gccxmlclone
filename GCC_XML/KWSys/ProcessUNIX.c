/*=========================================================================

Program:   KWSys - Kitware System Library
Module:    $RCSfile: ProcessUNIX.c,v $
Language:  C++
Date:      $Date: 2003-12-03 14:20:05 $
Version:   $Revision: 1.15 $

Copyright (c) 2002 Kitware, Inc., Insight Consortium.  All rights reserved.
See http://www.cmake.org/HTML/Copyright.html for details.

This software is distributed WITHOUT ANY WARRANTY; without even
the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
PURPOSE.  See the above copyright notices for more information.

=========================================================================*/
#define KWSYS_IN_PROCESS_C
#include "kwsysPrivate.h"
#include KWSYS_HEADER(Process.h)

/*

Implementation for UNIX

On UNIX, a child process is forked to exec the program.  Three
output pipes from the child are read by the parent process using a
select call to block until data are ready.  Two of the pipes are
stdout and stderr for the child.  The third is a special error pipe
that has two purposes.  First, if the child cannot exec the program,
the error is reported through the error pipe.  Second, the error
pipe is left open until the child exits.  This is used in
conjunction with the timeout on the select call to implement a
timeout for program even when it closes stdout and stderr.
*/

/*

TODO:

We cannot create the pipeline of processes in suspended states.  How
do we cleanup processes already started when one fails to load?  Right
now we are just killing them, which is probably not the right thing to
do.

*/

#include <stdio.h>     /* snprintf */
#include <stdlib.h>    /* malloc, free */
#include <string.h>    /* strdup, strerror, memset */
#include <sys/time.h>  /* struct timeval */
#include <sys/types.h> /* pid_t, fd_set */
#include <sys/wait.h>  /* waitpid */
#include <unistd.h>    /* pipe, close, fork, execvp, select, _exit */
#include <fcntl.h>     /* fcntl */
#include <errno.h>     /* errno */
#include <time.h>      /* gettimeofday */
#include <signal.h>    /* sigaction */

/* The number of pipes for the child's output.  The standard stdout
   and stderr pipes are the first two.  One more pipe is used to
   detect when the child process has terminated.  The third pipe is
   not given to the child process, so it cannot close it until it
   terminates.  */
#define KWSYSPE_PIPE_COUNT 3
#define KWSYSPE_PIPE_STDOUT 0
#define KWSYSPE_PIPE_STDERR 1
#define KWSYSPE_PIPE_TERM 2

/* The maximum amount to read from a pipe at a time.  */
#define KWSYSPE_PIPE_BUFFER_SIZE 1024

typedef struct timeval kwsysProcessTime;

typedef struct kwsysProcessCreateInformation_s
{
  int stdin;
  int stdout;
  int stderr;
  int term;
  int error[2];
} kwsysProcessCreateInformation;

/*--------------------------------------------------------------------------*/
static int kwsysProcessInitialize(kwsysProcess* cp);
static void kwsysProcessCleanup(kwsysProcess* cp, int error);
static void kwsysProcessCleanupDescriptor(int* pfd);
static int kwsysProcessCreate(kwsysProcess* cp, int index,
                              kwsysProcessCreateInformation* si, int* readEnd);
static int kwsysProcessGetTimeoutTime(kwsysProcess* cp, double* userTimeout,
                                      kwsysProcessTime* timeoutTime);
static int kwsysProcessGetTimeoutLeft(kwsysProcessTime* timeoutTime,
                                      kwsysProcessTime* timeoutLength);
static kwsysProcessTime kwsysProcessTimeGetCurrent();
static double kwsysProcessTimeToDouble(kwsysProcessTime t);
static kwsysProcessTime kwsysProcessTimeFromDouble(double d);
static int kwsysProcessTimeLess(kwsysProcessTime in1, kwsysProcessTime in2);
static kwsysProcessTime kwsysProcessTimeAdd(kwsysProcessTime in1, kwsysProcessTime in2);
static kwsysProcessTime kwsysProcessTimeSubtract(kwsysProcessTime in1, kwsysProcessTime in2);
static void kwsysProcessChildErrorExit(int errorPipe);
static void kwsysProcessRestoreDefaultSignalHandlers();

/*--------------------------------------------------------------------------*/
/* Structure containing data used to implement the child's execution.  */
struct kwsysProcess_s
{
  /* The command lines to execute.  */
  char*** Commands;
  int NumberOfCommands;

  /* Descriptors for the read ends of the child's output pipes. */
  int PipeReadEnds[KWSYSPE_PIPE_COUNT];

  /* Buffer for pipe data.  */
  char PipeBuffer[KWSYSPE_PIPE_BUFFER_SIZE];

  /* Process IDs returned by the calls to fork.  */
  pid_t* ForkPIDs;

  /* Flag for whether the children were terminated by a faild select.  */
  int SelectError;

  /* The timeout length.  */
  double Timeout;

  /* The working directory for the process. */
  char* WorkingDirectory;

  /* Time at which the child started.  Negative for no timeout.  */
  kwsysProcessTime StartTime;

  /* Time at which the child will timeout.  Negative for no timeout.  */
  kwsysProcessTime TimeoutTime;

  /* Flag for whether the timeout expired.  */
  int TimeoutExpired;

  /* The old SIGCHLD handler.  */
  struct sigaction OldSigChldAction;

  /* The number of pipes left open during execution.  */
  int PipesLeft;

  /* File descriptor set for call to select.  */
  fd_set PipeSet;

  /* The current status of the child process. */
  int State;

  /* The exceptional behavior that terminated the child process, if
   * any.  */
  int ExitException;

  /* The exit code of the child process.  */
  int ExitCode;

  /* The exit value of the child process, if any.  */
  int ExitValue;

  /* Whether the process was killed.  */
  int Killed;

  /* Buffer for error message in case of failure.  */
  char ErrorMessage[KWSYSPE_PIPE_BUFFER_SIZE+1];

  /* The exit codes of each child process in the pipeline.  */
  int* CommandExitCodes;
};

/*--------------------------------------------------------------------------*/
kwsysProcess* kwsysProcess_New()
{
  /* Allocate a process control structure.  */
  kwsysProcess* cp = (kwsysProcess*)malloc(sizeof(kwsysProcess));
  if(!cp)
    {
    return 0;
    }
  memset(cp, 0, sizeof(kwsysProcess));
  cp->State = kwsysProcess_State_Starting;
  return cp;
}

/*--------------------------------------------------------------------------*/
void kwsysProcess_Delete(kwsysProcess* cp)
{
  /* If the process is executing, wait for it to finish.  */
  if(cp->State == kwsysProcess_State_Executing)
    {
    kwsysProcess_WaitForExit(cp, 0);
    }

  /* Free memory.  */
  kwsysProcess_SetCommand(cp, 0);
  kwsysProcess_SetWorkingDirectory(cp, 0);
  if(cp->CommandExitCodes)
    {
    free(cp->CommandExitCodes);
    }
  free(cp);
}

/*--------------------------------------------------------------------------*/
int kwsysProcess_SetCommand(kwsysProcess* cp, char const* const* command)
{
  int i;
  for(i=0; i < cp->NumberOfCommands; ++i)
    {
    char** c = cp->Commands[i];
    while(*c)
      {
      free(*c++);
      }
    free(cp->Commands[i]);
    }
  cp->NumberOfCommands = 0;
  if(cp->Commands)
    {
    free(cp->Commands);
    cp->Commands = 0;
    }
  if(command)
    {
    return kwsysProcess_AddCommand(cp, command);
    }
  return 1;
}

/*--------------------------------------------------------------------------*/
int kwsysProcess_AddCommand(kwsysProcess* cp, char const* const* command)
{
  int newNumberOfCommands;
  char*** newCommands;

  /* Make sure we have a command to add.  */
  if(!command)
    {
    return 0;
    }

  /* Allocate a new array for command pointers.  */
  newNumberOfCommands = cp->NumberOfCommands + 1;
  if(!(newCommands = (char***)malloc(sizeof(char**) * newNumberOfCommands)))
    {
    /* Out of memory.  */
    return 0;
    }

  /* Copy any existing commands into the new array.  */
  {
  int i;
  for(i=0; i < cp->NumberOfCommands; ++i)
    {
    newCommands[i] = cp->Commands[i];
    }
  }

  /* Add the new command.  */
  {
  char const* const* c = command;
  int n = 0;
  int i = 0;
  while(*c++);
  n = c - command - 1;
  newCommands[cp->NumberOfCommands] = (char**)malloc((n+1)*sizeof(char*));
  if(!newCommands[cp->NumberOfCommands])
    {
    /* Out of memory.  */
    free(newCommands);
    return 0;
    }
  for(i=0; i < n; ++i)
    {
    newCommands[cp->NumberOfCommands][i] = strdup(command[i]);
    if(!newCommands[cp->NumberOfCommands][i])
      {
      break;
      }
    }
  if(i < n)
    {
    /* Out of memory.  */
    for(;i > 0; --i)
      {
      free(newCommands[cp->NumberOfCommands][i-1]);
      }
    free(newCommands);
    return 0;
    }
  newCommands[cp->NumberOfCommands][n] = 0;
  }

  /* Successfully allocated new command array.  Free the old array. */
  free(cp->Commands);
  cp->Commands = newCommands;
  cp->NumberOfCommands = newNumberOfCommands;

  return 1;
}

/*--------------------------------------------------------------------------*/
void kwsysProcess_SetTimeout(kwsysProcess* cp, double timeout)
{
  cp->Timeout = timeout;
  if(cp->Timeout < 0)
    {
    cp->Timeout = 0;
    }
}

/*--------------------------------------------------------------------------*/
void kwsysProcess_SetWorkingDirectory(kwsysProcess* cp, const char* dir)
{
  if(cp->WorkingDirectory == dir)
    {
    return;
    }
  if(cp->WorkingDirectory && dir && strcmp(cp->WorkingDirectory, dir) == 0)
    {
    return;
    }
  if(cp->WorkingDirectory)
    {
    free(cp->WorkingDirectory);
    cp->WorkingDirectory = 0;
    }
  if(dir)
    {
    cp->WorkingDirectory = (char*)malloc(strlen(dir) + 1);
    strcpy(cp->WorkingDirectory, dir);
    }
}

/*--------------------------------------------------------------------------*/
int kwsysProcess_GetOption(kwsysProcess* cp, int optionId)
{
  (void)cp;
  (void)optionId;
  return 0;
}

/*--------------------------------------------------------------------------*/
void kwsysProcess_SetOption(kwsysProcess* cp, int optionId, int value)
{
  (void)cp;
  (void)optionId;
  (void)value;
}

/*--------------------------------------------------------------------------*/
int kwsysProcess_GetState(kwsysProcess* cp)
{
  return cp->State;
}

/*--------------------------------------------------------------------------*/
int kwsysProcess_GetExitException(kwsysProcess* cp)
{
  return cp->ExitException;
}

/*--------------------------------------------------------------------------*/
int kwsysProcess_GetExitCode(kwsysProcess* cp)
{
  return cp->ExitCode;
}

/*--------------------------------------------------------------------------*/
int kwsysProcess_GetExitValue(kwsysProcess* cp)
{
  return cp->ExitValue;
}

/*--------------------------------------------------------------------------*/
const char* kwsysProcess_GetErrorString(kwsysProcess* cp)
{
  if(cp->State == kwsysProcess_State_Error)
    {
    return cp->ErrorMessage;
    }
  return 0;
}

/*--------------------------------------------------------------------------*/
void kwsysProcess_Execute(kwsysProcess* cp)
{
  int i;
  struct sigaction newSigChldAction;
  kwsysProcessCreateInformation si = {-1, -1, -1, -1, {-1, -1}};

  /* Do not execute a second copy simultaneously.  */
  if(cp->State == kwsysProcess_State_Executing)
    {
    return;
    }

  /* Initialize the control structure for a new process.  */
  if(!kwsysProcessInitialize(cp))
    {
    strcpy(cp->ErrorMessage, "Out of memory");
    cp->State = kwsysProcess_State_Error;
    return;
    }

  /* We want no special handling of SIGCHLD.  Repeat call until it is
     not interrupted.  */
  memset(&newSigChldAction, 0, sizeof(struct sigaction));
  newSigChldAction.sa_handler = SIG_DFL;
  while((sigaction(SIGCHLD, &newSigChldAction, &cp->OldSigChldAction) < 0) &&
        (errno == EINTR));

  /* Setup the stderr and termination pipes to be shared by all processes.  */
  for(i=KWSYSPE_PIPE_STDERR; i < KWSYSPE_PIPE_COUNT; ++i)
    {
    /* Create the pipe.  */
    int p[2];
    if(pipe(p) < 0)
      {
      kwsysProcessCleanup(cp, 1);
      return;
      }

    /* Store the pipe.  */
    cp->PipeReadEnds[i] = p[0];
    if(i == KWSYSPE_PIPE_STDERR)
      {
      si.stderr = p[1];
      }
    else
      {
      si.term = p[1];
      }

    /* Set close-on-exec flag on the pipe's ends.  */
    if((fcntl(p[0], F_SETFD, FD_CLOEXEC) < 0) ||
       (fcntl(p[1], F_SETFD, FD_CLOEXEC) < 0))
      {
      kwsysProcessCleanup(cp, 1);
      kwsysProcessCleanupDescriptor(&si.stderr);
      kwsysProcessCleanupDescriptor(&si.term);
      return;
      }
    }

  /* The timeout period starts now.  */
  cp->StartTime = kwsysProcessTimeGetCurrent();
  cp->TimeoutTime.tv_sec = -1;
  cp->TimeoutTime.tv_usec = -1;

  /* Create the pipeline of processes.  */
  {
  int readEnd = 0;
  for(i=0; i < cp->NumberOfCommands; ++i)
    {
    if(!kwsysProcessCreate(cp, i, &si, &readEnd))
      {
      kwsysProcessCleanup(cp, 1);

      /* Release resources that may have been allocated for this
         process before an error occurred.  */
      kwsysProcessCleanupDescriptor(&readEnd);
      if(i > 0)
        {
        kwsysProcessCleanupDescriptor(&si.stdin);
        }
      kwsysProcessCleanupDescriptor(&si.stdout);
      kwsysProcessCleanupDescriptor(&si.stderr);
      kwsysProcessCleanupDescriptor(&si.term);
      kwsysProcessCleanupDescriptor(&si.error[0]);
      kwsysProcessCleanupDescriptor(&si.error[1]);
      return;
      }
    }
  /* Save a handle to the output pipe for the last process.  */
  cp->PipeReadEnds[KWSYSPE_PIPE_STDOUT] = readEnd;
  }

  /* The parent process does not need the output pipe write ends.  */
  kwsysProcessCleanupDescriptor(&si.stderr);
  kwsysProcessCleanupDescriptor(&si.term);

  /* All the pipes are now open.  */
  cp->PipesLeft = KWSYSPE_PIPE_COUNT;

  /* The process has now started.  */
  cp->State = kwsysProcess_State_Executing;
}

/*--------------------------------------------------------------------------*/
int kwsysProcess_WaitForData(kwsysProcess* cp, int pipes, char** data,
                             int* length, double* userTimeout)
{
  int i;
  int max = -1;
  kwsysProcessTime* timeout = 0;
  kwsysProcessTime timeoutLength;
  kwsysProcessTime timeoutTime;
  kwsysProcessTime userStartTime;
  int user = 0;
  int expired = 0;
  int pipeId = 0;
  int numReady = 0;

  /* Record the time at which user timeout period starts.  */
  if(userTimeout)
    {
    userStartTime = kwsysProcessTimeGetCurrent();
    }

  /* Calculate the time at which a timeout will expire, and whether it
     is the user or process timeout.  */
  user = kwsysProcessGetTimeoutTime(cp, userTimeout, &timeoutTime);

  /* Data can only be available when pipes are open.  If the process
     is not running, cp->PipesLeft will be 0.  */
  while(cp->PipesLeft > 0)
    {
    /* Check for any open pipes with data reported ready by the last
       call to select.  */
    for(i=0; i < KWSYSPE_PIPE_COUNT; ++i)
      {
      if(cp->PipeReadEnds[i] >= 0 &&
         FD_ISSET(cp->PipeReadEnds[i], &cp->PipeSet))
        {
        int n;

        /* We are handling this pipe now.  Remove it from the set.  */
        FD_CLR(cp->PipeReadEnds[i], &cp->PipeSet);

        /* The pipe is ready to read without blocking.  Keep trying to
           read until the operation is not interrupted.  */
        while(((n = read(cp->PipeReadEnds[i], cp->PipeBuffer,
                         KWSYSPE_PIPE_BUFFER_SIZE)) < 0) && (errno == EINTR));
        if(n > 0)
          {
          /* We have data on this pipe.  */
          if(i == KWSYSPE_PIPE_TERM)
            {
            /* This is data on the special termination pipe.  Ignore it.  */
            }
          else if(pipes & (1 << i))
            {
            /* Caller wants this data.  Report it.  */
            *data = cp->PipeBuffer;
            *length = n;
            pipeId = (1 << i);
            break;
            }
          }
        else
          {
          /* We are done reading from this pipe.  */
          kwsysProcessCleanupDescriptor(&cp->PipeReadEnds[i]);
          --cp->PipesLeft;
          }
        }
      }

    /* If we have data, break early.  */
    if(pipeId)
      {
      break;
      }

    /* Make sure the set is empty (it should always be empty here
       anyway).  */
    FD_ZERO(&cp->PipeSet);

    /* Add the pipe reading ends that are still open.  */
    max = -1;
    for(i=0; i < KWSYSPE_PIPE_COUNT; ++i)
      {
      if(cp->PipeReadEnds[i] >= 0)
        {
        FD_SET(cp->PipeReadEnds[i], &cp->PipeSet);
        if(cp->PipeReadEnds[i] > max)
          {
          max = cp->PipeReadEnds[i];
          }
        }
      }

    /* Make sure we have a non-empty set.  */
    if(max < 0)
      {
      /* All pipes have closed.  Child has terminated.  */
      break;
      }

    /* Setup a timeout if required.  */
    if(timeoutTime.tv_sec < 0)
      {
      timeout = 0;
      }
    else
      {
      timeout = &timeoutLength;
      }
    if(kwsysProcessGetTimeoutLeft(&timeoutTime, &timeoutLength))
      {
      /* Timeout has already expired.  */
      expired = 1;
      break;
      }

    /* Run select to block until data are available.  Repeat call
       until it is not interrupted.  */
    while(((numReady = select(max+1, &cp->PipeSet, 0, 0, timeout)) < 0) &&
          (errno == EINTR));

    /* Check result of select.  */
    if(numReady == 0)
      {
      /* Select's timeout expired.  */
      expired = 1;
      break;
      }
    else if(numReady < 0)
      {
      /* Select returned an error.  Leave the error description in the
         pipe buffer.  */
      strncpy(cp->ErrorMessage, strerror(errno), KWSYSPE_PIPE_BUFFER_SIZE);

      /* Kill the children now.  */
      kwsysProcess_Kill(cp);
      cp->Killed = 0;
      cp->SelectError = 1;
      cp->PipesLeft = 0;
      }
    }

  /* Update the user timeout.  */
  if(userTimeout)
    {
    kwsysProcessTime userEndTime = kwsysProcessTimeGetCurrent();
    kwsysProcessTime difference = kwsysProcessTimeSubtract(userEndTime,
                                                     userStartTime);
    double d = kwsysProcessTimeToDouble(difference);
    *userTimeout -= d;
    if(*userTimeout < 0)
      {
      *userTimeout = 0;
      }
    }

  /* Check what happened.  */
  if(pipeId)
    {
    /* Data are ready on a pipe.  */
    return pipeId;
    }
  else if(expired)
    {
    /* A timeout has expired.  */
    if(user)
      {
      /* The user timeout has expired.  It has no time left.  */
      return kwsysProcess_Pipe_Timeout;
      }
    else
      {
      /* The process timeout has expired.  Kill the children now.  */
      kwsysProcess_Kill(cp);
      cp->Killed = 0;
      cp->TimeoutExpired = 1;
      cp->PipesLeft = 0;
      return 0;
      }
    }
  else
    {
    /* No pipes are left open.  */
    return 0;
    }
}

/*--------------------------------------------------------------------------*/
int kwsysProcess_WaitForExit(kwsysProcess* cp, double* userTimeout)
{
  int result = 0;
  int status = 0;
  int pipe = 0;

  /* Make sure we are executing a process.  */
  if(cp->State != kwsysProcess_State_Executing)
    {
    return 1;
    }

  /* Wait for all the pipes to close.  Ignore all data.  */
  while((pipe = kwsysProcess_WaitForData(cp, 0, 0, 0, userTimeout)) > 0)
    {
    if(pipe == kwsysProcess_Pipe_Timeout)
      {
      return 0;
      }
    }

  /* Wait for each child to terminate.  The process should have
     already exited because KWSYSPE_PIPE_TERM has been closed by this
     point.  Repeat the call until it is not interrupted.  */
  {
  int i;
  for(i=0; i < cp->NumberOfCommands; ++i)
    {
    while(((result = waitpid(cp->ForkPIDs[i],
                             &cp->CommandExitCodes[i], 0)) < 0) &&
          (errno == EINTR));
    if(result <= 0 && cp->State != kwsysProcess_State_Error)
      {
      /* Unexpected error.  Report the first time this happens.  */
      strncpy(cp->ErrorMessage, strerror(errno), KWSYSPE_PIPE_BUFFER_SIZE);
      cp->State = kwsysProcess_State_Error;
      }
    }
  }

  /* Check if there was an error in one of the waitpid calls.  */
  if(cp->State == kwsysProcess_State_Error)
    {
    /* The error message is already in its buffer.  Tell
       kwsysProcessCleanup to not create it.  */
    kwsysProcessCleanup(cp, 0);
    return 1;
    }

  /* Check whether the child reported an error invoking the process.  */
  if(cp->SelectError)
    {
    /* The error message is already in its buffer.  Tell
       kwsysProcessCleanup to not create it.  */
    kwsysProcessCleanup(cp, 0);
    cp->State = kwsysProcess_State_Error;
    return 1;
    }

  /* Use the status of the last process in the pipeline.  */
  status = cp->CommandExitCodes[cp->NumberOfCommands-1];

  /* Determine the outcome.  */
  if(cp->Killed)
    {
    /* We killed the child.  */
    cp->State = kwsysProcess_State_Killed;
    }
  else if(cp->TimeoutExpired)
    {
    /* The timeout expired.  */
    cp->State = kwsysProcess_State_Expired;
    }
  else if(WIFEXITED(status))
    {
    /* The child exited normally.  */
    cp->State = kwsysProcess_State_Exited;
    cp->ExitException = kwsysProcess_Exception_None;
    cp->ExitCode = status;
    cp->ExitValue = (int)WEXITSTATUS(status);
    }
  else if(WIFSIGNALED(status))
    {
    /* The child received an unhandled signal.  */
    cp->State = kwsysProcess_State_Exception;
    switch ((int)WTERMSIG(status))
      {
#ifdef SIGSEGV
      case SIGSEGV: cp->ExitException = kwsysProcess_Exception_Fault; break;
#endif
#ifdef SIGBUS
      case SIGBUS: cp->ExitException = kwsysProcess_Exception_Fault; break;
#endif
#ifdef SIGFPE
      case SIGFPE:  cp->ExitException = kwsysProcess_Exception_Numerical; break;
#endif
#ifdef SIGILL
      case SIGILL:  cp->ExitException = kwsysProcess_Exception_Illegal; break;
#endif
#ifdef SIGINT
      case SIGINT:  cp->ExitException = kwsysProcess_Exception_Interrupt; break;
#endif
      default: cp->ExitException = kwsysProcess_Exception_Other; break;
      }
    cp->ExitCode = status;
    }
  else
    {
    /* Error getting the child return code.  */
    strcpy(cp->ErrorMessage, "Error getting child return code.");
    cp->State = kwsysProcess_State_Error;
    }

  /* Normal cleanup.  */
  kwsysProcessCleanup(cp, 0);
  return 1;
}

/*--------------------------------------------------------------------------*/
void kwsysProcess_Kill(kwsysProcess* cp)
{
  int i;

  /* Make sure we are executing a process.  */
  if(cp->State != kwsysProcess_State_Executing)
    {
    return;
    }

  /* Kill the children.  */
  cp->Killed = 1;
  for(i=0; i < cp->NumberOfCommands; ++i)
    {
    if(cp->ForkPIDs[i])
      {
      kill(cp->ForkPIDs[i], SIGKILL);
      }
    }
}

/*--------------------------------------------------------------------------*/
/* Initialize a process control structure for kwsysProcess_Execute.  */
static int kwsysProcessInitialize(kwsysProcess* cp)
{
  int i;
  for(i=0; i < KWSYSPE_PIPE_COUNT; ++i)
    {
    cp->PipeReadEnds[i] = -1;
    }
  cp->SelectError = 0;
  cp->StartTime.tv_sec = -1;
  cp->StartTime.tv_usec = -1;
  cp->TimeoutTime.tv_sec = -1;
  cp->TimeoutTime.tv_usec = -1;
  cp->TimeoutExpired = 0;
  cp->PipesLeft = 0;
  FD_ZERO(&cp->PipeSet);
  cp->State = kwsysProcess_State_Starting;
  cp->Killed = 0;
  cp->ExitException = kwsysProcess_Exception_None;
  cp->ExitCode = 1;
  cp->ExitValue = 1;
  cp->ErrorMessage[0] = 0;

  if(cp->ForkPIDs)
    {
    free(cp->ForkPIDs);
    }
  cp->ForkPIDs = (pid_t*)malloc(sizeof(pid_t)*cp->NumberOfCommands);
  if(!cp->ForkPIDs)
    {
    return 0;
    }
  memset(cp->ForkPIDs, 0, sizeof(pid_t)*cp->NumberOfCommands);

  if(cp->CommandExitCodes)
    {
    free(cp->CommandExitCodes);
    }
  cp->CommandExitCodes = (int*)malloc(sizeof(int)*cp->NumberOfCommands);
  if(!cp->CommandExitCodes)
    {
    return 0;
    }
  memset(cp->CommandExitCodes, 0, sizeof(int)*cp->NumberOfCommands);

  return 1;
}

/*--------------------------------------------------------------------------*/
/* Free all resources used by the given kwsysProcess instance that were
   allocated by kwsysProcess_Execute.  */
static void kwsysProcessCleanup(kwsysProcess* cp, int error)
{
  int i;

  if(error)
    {
    /* We are cleaning up due to an error.  Report the error message
       if one has not been provided already.  */
    if(cp->ErrorMessage[0] == 0)
      {
      strncpy(cp->ErrorMessage, strerror(errno), KWSYSPE_PIPE_BUFFER_SIZE);
      }

    /* Set the error state.  */
    cp->State = kwsysProcess_State_Error;

    /* Kill any children already started.  */
    if(cp->ForkPIDs)
      {
      for(i=0; i < cp->NumberOfCommands; ++i)
        {
        if(cp->ForkPIDs[i])
          {
          kill(cp->ForkPIDs[i], SIGKILL);
          }
        }
      }
    }

  /* Restore the SIGCHLD handler.  */
  while((sigaction(SIGCHLD, &cp->OldSigChldAction, 0) < 0) &&
        (errno == EINTR));

  /* Free memory.  */
  if(cp->ForkPIDs)
    {
    free(cp->ForkPIDs);
    cp->ForkPIDs = 0;
    }

  /* Close pipe handles.  */
  for(i=0; i < KWSYSPE_PIPE_COUNT; ++i)
    {
    kwsysProcessCleanupDescriptor(&cp->PipeReadEnds[i]);
    }
}

/*--------------------------------------------------------------------------*/
/* Close the given file descriptor if it is open.  Reset its value to -1.  */
static void kwsysProcessCleanupDescriptor(int* pfd)
{
  if(pfd && *pfd >= 0)
    {
    /* Keep trying to close until it is not interrupted by a
     * signal.  */
    while((close(*pfd) < 0) && (errno == EINTR));
    *pfd = -1;
    }
}

/*--------------------------------------------------------------------------*/
int kwsysProcessCreate(kwsysProcess* cp, int index,
                       kwsysProcessCreateInformation* si, int* readEnd)
{
  /* Setup the process's stdin.  */
  if(index > 0)
    {
    si->stdin = *readEnd;
    *readEnd = 0;
    }
  else
    {
    si->stdin = 0;
    }

  /* Setup the process's stdout.  */
  {
  /* Create the pipe.  */
  int p[2];
  if(pipe(p) < 0)
    {
    return 0;
    }
  *readEnd = p[0];
  si->stdout = p[1];

  /* Set close-on-exec flag on the pipe's ends.  */
  if((fcntl(p[0], F_SETFD, FD_CLOEXEC) < 0) ||
     (fcntl(p[1], F_SETFD, FD_CLOEXEC) < 0))
    {
    return 0;
    }
  }

  /* Create the error reporting pipe.  */
  if(pipe(si->error) < 0)
    {
    return 0;
    }

  /* Set close-on-exec flag on the error pipe's write end.  */
  if(fcntl(si->error[1], F_SETFD, FD_CLOEXEC) < 0)
    {
    return 0;
    }

  /* Fork off a child process.  */
  cp->ForkPIDs[index] = fork();
  if(cp->ForkPIDs[index] < 0)
    {
    return 0;
    }

  if(cp->ForkPIDs[index] == 0)
    {
    /* Close the read end of the error reporting pipe.  */
    close(si->error[0]);

    /* Setup the stdin, stdout, and stderr pipes.  */
    if(index > 0)
      {
      dup2(si->stdin, 0);
      }
    dup2(si->stdout, 1);
    dup2(si->stderr, 2);

    /* Clear the close-on-exec flag for stdin, stdout, and stderr.
       Also clear it for the termination pipe.  All other pipe handles
       will be closed when exec succeeds.  */
    fcntl(0, F_SETFD, 0);
    fcntl(1, F_SETFD, 0);
    fcntl(2, F_SETFD, 0);
    fcntl(si->term, F_SETFD, 0);

    /* Restore all default signal handlers. */
    kwsysProcessRestoreDefaultSignalHandlers();

    /* Change to the working directory specified, if any.  */
    if(cp->WorkingDirectory)
      {
      /* Some platforms specify that the chdir call may be
         interrupted.  Repeat the call until it finishes.  */
      int r;
      while(((r = chdir(cp->WorkingDirectory)) < 0) && (errno == EINTR));
      if(r < 0)
        {
        /* Failure.  Report error to parent and terminate.  */
        kwsysProcessChildErrorExit(si->error[1]);
        }
      }

    /* Execute the real process.  If successful, this does not return.  */
    execvp(cp->Commands[index][0], cp->Commands[index]);

    /* Failure.  Report error to parent and terminate.  */
    kwsysProcessChildErrorExit(si->error[1]);
    }

  /* We are done with the error reporting pipe write end.  */
  kwsysProcessCleanupDescriptor(&si->error[1]);

  /* Block until the child's exec call succeeds and closes the error
     pipe or writes data to the pipe to report an error.  */
  {
  int total = 0;
  int n = 1;
  /* Read the entire error message up to the length of our buffer.  */
  while(total < KWSYSPE_PIPE_BUFFER_SIZE && n > 0)
    {
    /* Keep trying to read until the operation is not interrupted.  */
    while(((n = read(si->error[0], cp->ErrorMessage+total,
                     KWSYSPE_PIPE_BUFFER_SIZE-total)) < 0) &&
          (errno == EINTR));
    if(n > 0)
      {
      total += n;
      }
    }

  /* We are done with the error reporting pipe read end.  */
  kwsysProcessCleanupDescriptor(&si->error[0]);

  if(total > 0)
    {
    /* The child failed to execute the process.  */
    return 0;
    }
  }

  /* Successfully created this child process.  */
  if(index > 0)
    {
    /* The parent process does not need the input pipe read end.  */
    kwsysProcessCleanupDescriptor(&si->stdin);
    }

  /* The parent process does not need the output pipe write ends.  */
  kwsysProcessCleanupDescriptor(&si->stdout);

  return 1;
}

/*--------------------------------------------------------------------------*/
/* Get the time at which either the process or user timeout will
   expire.  Returns 1 if the user timeout is first, and 0 otherwise.  */
static int kwsysProcessGetTimeoutTime(kwsysProcess* cp, double* userTimeout,
                                      kwsysProcessTime* timeoutTime)
{
  /* The first time this is called, we need to calculate the time at
     which the child will timeout.  */
  if(cp->Timeout && cp->TimeoutTime.tv_sec < 0)
    {
    kwsysProcessTime length = kwsysProcessTimeFromDouble(cp->Timeout);
    cp->TimeoutTime = kwsysProcessTimeAdd(cp->StartTime, length);
    }

  /* Start with process timeout.  */
  *timeoutTime = cp->TimeoutTime;

  /* Check if the user timeout is earlier.  */
  if(userTimeout)
    {
    kwsysProcessTime currentTime = kwsysProcessTimeGetCurrent();
    kwsysProcessTime userTimeoutLength = kwsysProcessTimeFromDouble(*userTimeout);
    kwsysProcessTime userTimeoutTime = kwsysProcessTimeAdd(currentTime,
                                                     userTimeoutLength);
    if(kwsysProcessTimeLess(userTimeoutTime, *timeoutTime))
      {
      *timeoutTime = userTimeoutTime;
      return 1;
      }
    }
  return 0;
}

/*--------------------------------------------------------------------------*/
/* Get the length of time before the given timeout time arrives.
   Returns 1 if the time has already arrived, and 0 otherwise.  */
static int kwsysProcessGetTimeoutLeft(kwsysProcessTime* timeoutTime,
                                      kwsysProcessTime* timeoutLength)
{
  if(timeoutTime->tv_sec < 0)
    {
    /* No timeout time has been requested.  */
    return 0;
    }
  else
    {
    /* Calculate the remaining time.  */
    kwsysProcessTime currentTime = kwsysProcessTimeGetCurrent();
    *timeoutLength = kwsysProcessTimeSubtract(*timeoutTime, currentTime);
    if(timeoutLength->tv_sec < 0)
      {
      /* Timeout has already expired.  */
      return 1;
      }
    else
      {
      /* There is some time left.  */
      return 0;
      }
    }
}

/*--------------------------------------------------------------------------*/
static kwsysProcessTime kwsysProcessTimeGetCurrent()
{
  kwsysProcessTime current;
  gettimeofday(&current, 0);
  return current;
}

/*--------------------------------------------------------------------------*/
static double kwsysProcessTimeToDouble(kwsysProcessTime t)
{
  return (double)t.tv_sec + t.tv_usec*0.000001;
}

/*--------------------------------------------------------------------------*/
static kwsysProcessTime kwsysProcessTimeFromDouble(double d)
{
  kwsysProcessTime t;
  t.tv_sec = (long)d;
  t.tv_usec = (long)((d-t.tv_sec)*1000000);
  return t;
}

/*--------------------------------------------------------------------------*/
static int kwsysProcessTimeLess(kwsysProcessTime in1, kwsysProcessTime in2)
{
  return ((in1.tv_sec < in2.tv_sec) ||
          ((in1.tv_sec == in2.tv_sec) && (in1.tv_usec < in2.tv_usec)));
}

/*--------------------------------------------------------------------------*/
static kwsysProcessTime kwsysProcessTimeAdd(kwsysProcessTime in1, kwsysProcessTime in2)
{
  kwsysProcessTime out;
  out.tv_sec = in1.tv_sec + in2.tv_sec;
  out.tv_usec = in1.tv_usec + in2.tv_usec;
  if(out.tv_usec > 1000000)
    {
    out.tv_usec -= 1000000;
    out.tv_sec += 1;
    }
  return out;
}

/*--------------------------------------------------------------------------*/
static kwsysProcessTime kwsysProcessTimeSubtract(kwsysProcessTime in1, kwsysProcessTime in2)
{
  kwsysProcessTime out;
  out.tv_sec = in1.tv_sec - in2.tv_sec;
  out.tv_usec = in1.tv_usec - in2.tv_usec;
  if(out.tv_usec < 0)
    {
    out.tv_usec += 1000000;
    out.tv_sec -= 1;
    }
  return out;
}

/*--------------------------------------------------------------------------*/
/* When the child process encounters an error before its program is
   invoked, this is called to report the error to the parent and
   exit.  */
static void kwsysProcessChildErrorExit(int errorPipe)
{
  /* Construct the error message.  */
  char buffer[KWSYSPE_PIPE_BUFFER_SIZE];
  strncpy(buffer, strerror(errno), KWSYSPE_PIPE_BUFFER_SIZE);

  /* Report the error to the parent through the special pipe.  */
  write(errorPipe, buffer, strlen(buffer));

  /* Terminate without cleanup.  */
  _exit(1);
}

/*--------------------------------------------------------------------------*/
/* Restores all signal handlers to their default values.  */
static void kwsysProcessRestoreDefaultSignalHandlers()
{
  struct sigaction act;
  memset(&act, 0, sizeof(struct sigaction));
  act.sa_handler = SIG_DFL;
#ifdef SIGHUP
  sigaction(SIGHUP, &act, 0);
#endif
#ifdef SIGINT
  sigaction(SIGINT, &act, 0);
#endif
#ifdef SIGQUIT
  sigaction(SIGQUIT, &act, 0);
#endif
#ifdef SIGILL
  sigaction(SIGILL, &act, 0);
#endif
#ifdef SIGTRAP
  sigaction(SIGTRAP, &act, 0);
#endif
#ifdef SIGABRT
  sigaction(SIGABRT, &act, 0);
#endif
#ifdef SIGIOT
  sigaction(SIGIOT, &act, 0);
#endif
#ifdef SIGBUS
  sigaction(SIGBUS, &act, 0);
#endif
#ifdef SIGFPE
  sigaction(SIGFPE, &act, 0);
#endif
#ifdef SIGUSR1
  sigaction(SIGUSR1, &act, 0);
#endif
#ifdef SIGSEGV
  sigaction(SIGSEGV, &act, 0);
#endif
#ifdef SIGUSR2
  sigaction(SIGUSR2, &act, 0);
#endif
#ifdef SIGPIPE
  sigaction(SIGPIPE, &act, 0);
#endif
#ifdef SIGALRM
  sigaction(SIGALRM, &act, 0);
#endif
#ifdef SIGTERM
  sigaction(SIGTERM, &act, 0);
#endif
#ifdef SIGSTKFLT
  sigaction(SIGSTKFLT, &act, 0);
#endif
#ifdef SIGCLD
  sigaction(SIGCLD, &act, 0);
#endif
#ifdef SIGCHLD
  sigaction(SIGCHLD, &act, 0);
#endif
#ifdef SIGCONT
  sigaction(SIGCONT, &act, 0);
#endif
#ifdef SIGTSTP
  sigaction(SIGTSTP, &act, 0);
#endif
#ifdef SIGTTIN
  sigaction(SIGTTIN, &act, 0);
#endif
#ifdef SIGTTOU
  sigaction(SIGTTOU, &act, 0);
#endif
#ifdef SIGURG
  sigaction(SIGURG, &act, 0);
#endif
#ifdef SIGXCPU
  sigaction(SIGXCPU, &act, 0);
#endif
#ifdef SIGXFSZ
  sigaction(SIGXFSZ, &act, 0);
#endif
#ifdef SIGVTALRM
  sigaction(SIGVTALRM, &act, 0);
#endif
#ifdef SIGPROF
  sigaction(SIGPROF, &act, 0);
#endif
#ifdef SIGWINCH
  sigaction(SIGWINCH, &act, 0);
#endif
#ifdef SIGPOLL
  sigaction(SIGPOLL, &act, 0);
#endif
#ifdef SIGIO
  sigaction(SIGIO, &act, 0);
#endif
#ifdef SIGPWR
  sigaction(SIGPWR, &act, 0);
#endif
#ifdef SIGSYS
  sigaction(SIGSYS, &act, 0);
#endif
#ifdef SIGUNUSED
  sigaction(SIGUNUSED, &act, 0);
#endif
}
