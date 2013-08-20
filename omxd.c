/* (c) SZABO Gergely <szg@subogero.com>, license GPLv3 */
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <signal.h>
#include <string.h>
#include <stdlib.h>
#include "omxd.h"

int logfd;
static int ctrlpipe[2];
static pid_t player_pid = 0;

static int client(int argc, char *argv[]);
static int daemonize(void);
static int parse(char *line);
static int player(char *cmd, char *file);
static void player_quit(int signum); /* SIGCHLD signal handler */

int main(int argc, char *argv[])
{
	/* Client when called with options */
	if (argc > 1) {
		return client(argc, argv);
	}
	int daemon_error = daemonize();
	if (daemon_error > 0)
		return daemon_error;
	else if (daemon_error < 0)
		return 0;
	/* Main loop */
	int cmdfd = -1;
	while (1) {
		if (cmdfd < 0) {
			cmdfd = open("omxd.cmd", O_RDONLY);
			if (cmdfd < 0) {
				writestr(logfd, "Can't open omxd.cmd\n");
				return 7;
			} else {
				writestr(logfd, "Client opened omxd.cmd\n");
				continue;
			}
		}
		char line[LINE_LENGTH];
		int len = read(cmdfd, line, LINE_MAX);
		if (len == 0) {
			writestr(logfd, "Client closed omxd.cmd\n");
			close(cmdfd);
			cmdfd = -1;
			continue;
		}
		/* Make C-string from one input line, discard LF and rest*/
		line[LINE_MAX] = 0;
		char *lf = strchr(line, '\n');
		if (lf != NULL) {
			*lf = 0;
		}
		parse(line);
	}
	return 0;
}

/* Simple client */
static int client(int argc, char *argv[])
{
	int cmdfd = open("/var/run/omxd.cmd", O_WRONLY|O_NONBLOCK);
	if (cmdfd < 0) {
		writestr(2, "Can't open /var/run/omxd.cmd\n");
		return 10;
	}
	if (writestr(cmdfd, argv[1]) == 0) {
		writestr(2, "Can't write /var/run/omxd.cmd\n");
		return 11;
	}
	return 0;
}

/* Fork, umask, SID, chdir, close, logfile, FIFO */
static int daemonize(void)
{
	/* Fork the real daemon */
	pid_t pid = fork();
	if (pid < 0)
		return 1;
	if (pid > 0) {
		writestr(1, "omxd daemon started, PID ");
		writedec(1, pid);
		writestr(1, "\n");
		return -1;
	}
	/* umask and session ID */
	umask(0);
	pid_t sid = setsid();
	if (sid < 0)
		return 2;
	/* Run in erm... /var/run */
	//if (chdir("/var/run/") < 0)
	//	return 3;
	/* Create log file as stdout and stderr */
	close(0);
	close(1);
	close(2);
	logfd = creat("omxd.log", 0644);
	if (logfd < 0)
		return 4;
	writedec(logfd, sid);
	if (writestr(logfd, " omxd started\n") == 0)
		return 5;
	/* Create and open FIFO for command input as stdin */
	unlink("omxd.cmd");
	writestr(logfd, "Deleted original omxd.cmd FIFO\n");
	if (mknod("omxd.cmd", S_IFIFO | 0666, 0) < 0)
		return 6;
	writestr(logfd, "Created new omxd.cmd FIFO\n");
	return 0;
}

/* Get command char and file/URL name from a command line */
static int parse(char *line)
{
	/* Extract command and file/URL from line */
	char *cmd = NULL;
	char *file = line;
	while (1) {
		if (*file == 0) {
			if (cmd == NULL)
				cmd = line;
			break;
		} else if (*file == ' ' || *file == '\t') {
			if (cmd == NULL) {
				*file = 0;
				cmd = line;
			}
		} else if (cmd != NULL) {
			break;
		}
		file++;
	}
	if (cmd != NULL) {
		player(cmd, playlist(cmd, file));
	}
	return cmd != NULL ? *cmd : 0;
}

/* Control the actual omxplayer */
static int player(char *cmd, char *file)
{
	if (file != NULL && *file != 0) {
		if (player_pid != 0) {
			write(ctrlpipe[1], "q", 1);
			player_quit(0);
		}
		pipe(ctrlpipe);
		player_pid = fork();
		if (player_pid < 0) {
			player_pid = 0;
			close(ctrlpipe[0]);
			close(ctrlpipe[1]);
		} else if (player_pid > 0) {
			close(ctrlpipe[0]);
			signal(SIGCHLD, player_quit);
		} else {
			close(ctrlpipe[1]);
			if (logfd == 0)
				logfd = dup(logfd);
			close(0);
			dup(ctrlpipe[0]);
			close(ctrlpipe[0]);
			char *argv[4];
			argv[0] = "/usr/bin/omxplayer";
			argv[1] = "-olocal";
			argv[2] = file;
			argv[3] = NULL;
			writestr(logfd, file);
			writestr(logfd, "\n");
			execve(argv[0], argv, NULL);
			writestr(logfd, "Unable to exec omxplayer\n");
			exit(20);
		}
	} else if (player_pid != 0) {
		writestr(logfd, "Send command to omxplayer: ");
		writestr(logfd, cmd);
		write(logfd, "\n", 1);
		if      (*cmd == 'F')
			strcpy(cmd, "\033[A");
		else if (*cmd == 'R')
			strcpy(cmd, "\033[B");
		else if (*cmd == 'f')
			strcpy(cmd, "\033[C");
		else if (*cmd == 'r')
			strcpy(cmd, "\033[D");
		writestr(ctrlpipe[1], cmd);
	}
}

/* Signal handler for SIGCHLD when player exits */
static void player_quit(int signum)
{
	wait(NULL);
	close(ctrlpipe[1]);
	player_pid = 0;
	writestr(logfd, "omxplayer has quit\n");
	if (signum == SIGCHLD) {
		player("n", playlist("n", NULL));
		writestr(logfd, "Next file started\n");
	}
}

/* Write number in decimal format to file descriptor, printf() is BLOATED!!! */
int writedec(int fd, int num)
{
	int bytes = 0;
	/* Special case: negative numbers (print neg.sign) */
	if (num < 0) {
		write(fd, "-", 1);
		num *= -1;
		bytes++;
	}
	/*
	 * If num >= 10, print More Significant DigitS first by recursive call
	 * then we print Least Significatn Digit ourselves.
	 */
	int msds = num / 10;
	int lsd = num % 10;
	if (msds)
		bytes += writedec(fd, msds);
	char digit = '0' + lsd;
	write(fd, &digit, 1);
	return ++bytes;
}

/* Write a C-string to a file descriptor */
int writestr(int fd, char *str)
{
	int len = strlen(str);
	return write(fd, str, len);
}
