#include <iostream>
#include <list>
#include <iterator>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <cstring>
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>



/*
This program does the following.
1) Create handlers for two signals.
2) Create an idle process which will be executed when there is nothing
   else to do.
3) Create a send_signals process that sends a SIGALRM every so often

When run, it should produce the following output (approximately):

$ ./a.out
in CPU.cc at 247 main pid = 26428
state:    1
name:     IDLE
pid:      26430
ppid:     0
slices:   0
switches: 0
started:  0
in CPU.cc at 100 at beginning of send_signals getpid () = 26429
in CPU.cc at 216 idle getpid () = 26430
in CPU.cc at 222 going to sleep
in CPU.cc at 106 sending signal = 14
in CPU.cc at 107 to pid = 26428
in CPU.cc at 148 stopped running->pid = 26430
in CPU.cc at 155 continuing tocont->pid = 26430
in CPU.cc at 106 sending signal = 14
in CPU.cc at 107 to pid = 26428
in CPU.cc at 148 stopped running->pid = 26430
in CPU.cc at 155 continuing tocont->pid = 26430
in CPU.cc at 106 sending signal = 14
in CPU.cc at 107 to pid = 26428
in CPU.cc at 115 at end of send_signals
Terminated
---------------------------------------------------------------------------
Add the following functionality.
1) Change the NUM_SECONDS to 20.

2) Take any number of arguments for executables, and place each on new_list.
    The executable will not require arguments themselves.

3) When a SIGALRM arrives, scheduler() will be called. It calls
    choose_process which currently always returns the idle process. Do the
    following.
    a) Update the PCB for the process that was interrupted including the
        number of context switches and interrupts it had, and changing its
        state from RUNNING to READY.
    b) If there are any processes on the new_list, do the following.
        i) Take the one off the new_list and put it on the processes list.
        ii) Change its state to RUNNING, and fork() and execl() it.
    c) Modify choose_process to round robin the processes in the processes
        queue that are READY. If no process is READY in the queue, execute
        the idle process.

4) When a SIGCHLD arrives notifying that a child has exited, process_done() is
    called. process_done() currently only prints out the PID and the status.
    a) Add the printing of the information in the PCB including the number
        of times it was interrupted, the number of times it was context
        switched (this may be fewer than the interrupts if a process
        becomes the only non-idle process in the ready queue), and the total
        system time the process took.
    b) Change the state to TERMINATED.
    c) Start the idle process to use the rest of the time slice.
*/

#define NUM_SECONDS 20

// make sure the asserts work
#undef NDEBUG
#include <assert.h>

#define EBUG
#ifdef EBUG
#   define dmess(a) cout << "in " << __FILE__ << \
    " at " << __LINE__ << " " << a << endl;

#   define dprint(a) cout << "in " << __FILE__ << \
    " at " << __LINE__ << " " << (#a) << " = " << a << endl;

#   define dprintt(a,b) cout << "in " << __FILE__ << \
    " at " << __LINE__ << " " << a << " " << (#b) << " = " \
    << b << endl
#else
#   define dprint(a)
#endif /* EBUG */


//ADDED FOR ASSIGNMENT 5***********************************
#define READ_END 0
#define WRITE_END 1

#define NUM_CHILDREN 5
#define NUM_PIPES NUM_CHILDREN*2

#define P2K i
#define K2P i+1

#define WRITE(a) { const char *foo = a; write (1, foo, strlen (foo)); }


int child_count = 0;
//***********************************************************

using namespace std;

enum STATE { NEW, RUNNING, WAITING, READY, TERMINATED };

/*
** a signal handler for those signals delivered to this process, but
** not already handled.
*/
void grab (int signum) { dprint (signum); }

// c++decl> declare ISV as array 32 of pointer to function (int) returning
// void
void (*ISV[32])(int) = {
/*        00    01    02    03    04    05    06    07    08    09 */
/*  0 */ grab, grab, grab, grab, grab, grab, grab, grab, grab, grab,
/* 10 */ grab, grab, grab, grab, grab, grab, grab, grab, grab, grab,
/* 20 */ grab, grab, grab, grab, grab, grab, grab, grab, grab, grab,
/* 30 */ grab, grab
};

struct PCB
{
    STATE state;
    const char *name;   // name of the executable
    int pid;            // process id from fork();
    int ppid;           // parent process id
    int interrupts;     // number of times interrupted
    int switches;       // may be < interrupts
    int started;        // the time this process started
    
    //ADDED
    int pipes[NUM_PIPES][2]; 
};

/*
** an overloaded output operator that prints a PCB
*/
ostream& operator << (ostream &os, struct PCB *pcb)
{
    os << "state:        " << pcb->state << endl;
    os << "name:         " << pcb->name << endl;
    os << "pid:          " << pcb->pid << endl;
    os << "ppid:         " << pcb->ppid << endl;
    os << "interrupts:   " << pcb->interrupts << endl;
    os << "switches:     " << pcb->switches << endl;
    os << "started:      " << pcb->started << endl;
    return (os);
}

/*
** an overloaded output operator that prints a list of PCBs
*/
ostream& operator << (ostream &os, list<PCB *> which)
{
    list<PCB *>::iterator PCB_iter;
    for (PCB_iter = which.begin(); PCB_iter != which.end(); PCB_iter++)
    {
        os << (*PCB_iter);
    }
    return (os);
}


PCB *running;
PCB *idle;

// http://www.cplusplus.com/reference/list/list/
list<PCB *> new_list;
list<PCB *> processes;

int sys_time;

/*
**  send signal to process pid every interval for number of times.
*/
void send_signals (int signal, int pid, int interval, int number)
{
    dprintt ("at beginning of send_signals", getpid ());

    for (int i = 1; i <= number; i++)
    {
        sleep (interval);

        dprintt ("sending", signal);
        dprintt ("to", pid);

        if (kill (pid, signal) == -1)
        {
            perror ("kill");
            return;
        }
    }
    dmess ("at end of send_signals");
}

int eye2eh (int i, char *buf, int bufsize, int base)
{
    if (bufsize < 1) return (-1);
    buf[bufsize-1] = '\0';
    if (bufsize == 1) return (0);
    if (base < 2 || base > 16)
    {
        for (int j = bufsize-2; j >= 0; j--)
        {
            buf[j] = ' ';
        }
        return (-1);
    }

    int count = 0;
    const char *digits = "0123456789ABCDEF";
    for (int j = bufsize-2; j >= 0; j--)
    {
        if (i == 0)
        {
            buf[j] = ' ';
        }
        else
        {
            buf[j] = digits[i%base];
            i = i/base;
            count++;
        }
    }
    if (i != 0) return (-1);
    return (count);
}

struct sigaction *create_handler (int signum, void (*handler)(int))
{
    struct sigaction *action = new (struct sigaction);

    action->sa_handler = handler;
/*
**  SA_NOCLDSTOP
**  If  signum  is  SIGCHLD, do not receive notification when
**  child processes stop (i.e., when child processes  receive
**  one of SIGSTOP, SIGTSTP, SIGTTIN or SIGTTOU).
*/
    if (signum == SIGCHLD)
    {
        action->sa_flags = SA_NOCLDSTOP;
    }
    else
    {
        action->sa_flags = 0;
    }

    sigemptyset (&(action->sa_mask));

    assert (sigaction (signum, action, NULL) == 0);
    return (action);
}

PCB* choose_process ()
{
	
	running->interrupts ++;
	
	
	char path[100];
	
	//cout<<path<<endl;
	if(!new_list.empty()){
		running->state = READY;
		strcpy(path, "./");
		strcat(path, new_list.front()->name);

		int pid = fork();
		if(pid < 0){
			//ERROR DO THING HERE
		}


		if(pid == 0){
			for (int i = 0; i < NUM_PIPES; i+=2)
			{
				int child;
				if ((child = fork()) == 0)
				{
				    close (process->pipes[P2K][READ_END]);
				    close (process->pipes[K2P][WRITE_END]);

				    // assign fildes 3 and 4 to the pipe ends in the child
				    dup2 (process->pipes[P2K][WRITE_END], 3);
				    dup2 (process->pipes[K2P][READ_END], 4);

				    execl(path, new_list.front()->name, (char*) NULL);
				}
			}
			

		}else{
			new_list.front()->state = RUNNING;
			new_list.front()->pid = pid;
			new_list.front()->ppid = getpid();
			cout<<"UPDATED CONTEXT SWITCH IN NEW_LIST of "<< running->pid <<endl;
			running->switches++;
			new_list.front()->started = sys_time;
			
			processes.push_back(new_list.front());
			new_list.pop_front();
			running = processes.back();
			
			return running;
		}	
	}else{
		
		list<PCB*>::iterator it;
		for(it = processes.begin(); it != processes.end(); it++){
			if((*it)->state == RUNNING){
				processes.push_back(running);
				processes.back()->state = READY;
				processes.erase(it);
				break;
			}
		}

		for(it = processes.begin(); it != processes.end(); it++){
			
			if((*it)->state == READY){
				cout<<"UPDATED SWTICH IN ROUND ROBIN "<< (*it)->pid <<endl;
				if((*it)->pid != running->pid){
					running->switches ++;
				}
				running = *it;
				running->state = RUNNING;
				return running;
			}
		}
		/*running->state = READY;
		for(it = processes.begin(); it != proccesses.end(); it++){
			if((*it)->state == READY){
				if((*it)->pid != running->pid){
					running->switches++;
				}
				processes.erase(it)
			}
		}*/
		
		return running;
		
		
	}	

	return idle;
}

void process_trap (int signum)
{
    assert (signum == SIGTRAP);
    WRITE("---- entering process_trap\n");

    /*
    ** poll all the pipes as we don't know which process sent the trap, nor
    ** if more than one has arrived.
    */
    for (int i = 0; i < NUM_PIPES; i+=2)
    {
        char buf[1024];
        int num_read = read (running->pipes[P2K][READ_END], buf, 1023);
        if (num_read > 0)
        {
            buf[num_read] = '\0';
            WRITE("kernel read: ");
            WRITE(buf);
            WRITE("\n");

            // respond
            const char *message = "from the kernel to the process";
            write (running->pipes[K2P][WRITE_END], message, strlen (message));
        }
    }
    WRITE("---- leaving process_trap\n");
}

void scheduler (int signum)
{
    assert (signum == SIGALRM);
    sys_time++;

    PCB* tocont = choose_process();

    dprintt ("continuing", tocont->pid);
    if (kill (tocont->pid, SIGCONT) == -1)
    {
        perror ("kill");
        return;
    }
}

void process_done (int signum)
{

	assert (signum == SIGCHLD);
    WRITE("---- entering child_done\n");

    for (;;)
    {
        int status, cpid;
        cpid = waitpid (-1, &status, WNOHANG);
		cout<<"interrupts: " << running->interrupts<<endl;
		cout<<"context switches: "<< running->switches<<endl;
		cout<<"process time: "<< sys_time - running->started<<endl;
		

        if (cpid < 0)
        {
            WRITE("cpid < 0\n");
            kill (0, SIGTERM);
        }
        else if (cpid == 0)
        {
            WRITE("cpid == 0\n");
            break;
        }
        else
        {
            char buf[10];
            assert (eye2eh (cpid, buf, 10, 10) != -1);
            WRITE("process exited:");
            WRITE(buf);
            WRITE("\n");
            child_count++;
            if (child_count == NUM_CHILDREN)
            {
                kill (0, SIGTERM);
            }
        }
		
    }
	running->state = TERMINATED;
	running = idle;

    WRITE("---- leaving child_done\n");
/*
    assert (signum == SIGCHLD);

    int status, cpid;

    cpid = waitpid (-1, &status, WNOHANG);

    dprintt ("in process_done", cpid);

    if  (cpid == -1)
    {
        perror ("waitpid");
    }
    else if (cpid == 0)
    {
        if (errno == EINTR) { return; }
        perror ("no children");
    }
    else
    {
        dprint (WEXITSTATUS (status));
    }

	cout<<"interrupts: " << running->interrupts<<endl;
	cout<<"context switches: "<< running->switches<<endl;
	cout<<"process time: "<< sys_time - running->started<<endl;
	running->state = TERMINATED;
	running = idle;
*/
}

/*
** stop the running process and index into the ISV to call the ISR
*/
void ISR (int signum)
{
    if (kill (running->pid, SIGSTOP) == -1)
    {
        perror ("kill");
        return;
    }
    dprintt ("stopped", running->pid);

    ISV[signum](signum);
}

/*
** set up the "hardware"
*/
void boot (int pid)
{
    ISV[SIGALRM] = scheduler;       create_handler (SIGALRM, ISR);
    ISV[SIGCHLD] = process_done;    create_handler (SIGCHLD, ISR);
//*******************************ADDED******************************
	ISV[SIGTRAP] = process_trap;	create_handler (SIGTRAP, ISR);
//******************************************************************

    // start up clock interrupt
    int ret;
    if ((ret = fork ()) == 0)
    {
        // signal this process once a second for three times
        send_signals (SIGALRM, pid, 1, NUM_SECONDS);

        // once that's done, really kill everything...
        kill (0, SIGTERM);
    }

    if (ret < 0)
    {
        perror ("fork");
    }
}

void create_idle ()
{
    int idlepid;

    if ((idlepid = fork ()) == 0)
    {
        dprintt ("idle", getpid ());

        // the pause might be interrupted, so we need to
        // repeat it forever.
        for (;;)
        {
            dmess ("going to sleep");
            pause ();
            if (errno == EINTR)
            {
                dmess ("waking up");
                continue;
            }
            perror ("pause");
        }
    }
    idle = new (PCB);
    idle->state = RUNNING;
    idle->name = "IDLE";
    idle->pid = idlepid;
    idle->ppid = 0;
    idle->interrupts = 0;
    idle->switches = 0;
    idle->started = sys_time;
}

void create_list(int argc, char** argv){
	
	for(int i = 1; i < argc; i ++){
		PCB* process = new (PCB);
		process->state = NEW;
		process->name = argv[i];

		// create the pipes
    		for (int i = 0; i < NUM_PIPES; i+=2)
		{
			// i is from process to kernel, K2P from kernel to process
			assert (pipe (process->pipes[P2K]) == 0);
			assert (pipe (process->pipes[K2P]) == 0);

			// make the read end of the kernel pipe non-blocking.
			assert (fcntl (process->pipes[P2K][READ_END], F_SETFL,
			fcntl(process->pipes[P2K][READ_END], F_GETFL) | O_NONBLOCK) == 0);
		}

		
		
		new_list.push_back(process);
		cout<<process->name<<endl;



	}
	
}


int main (int argc, char **argv)
{
    int pid = getpid();
    dprintt ("main", pid);

    create_list(argc, argv);

    sys_time = 0;

    boot (pid);

    // create a process to soak up cycles
    create_idle ();
    running = idle;

    cout << running;

    // we keep this process around so that the children don't die and
    // to keep the IRQs in place.
    for (;;)
    {
        pause();
        if (errno == EINTR) { continue; }
        perror ("pause");
    }
}

