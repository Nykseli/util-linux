/*
 * sulogin	This program gives Linux machines a reasonable
 *		secure way to boot single user. It forces the
 *		user to supply the root password before a
 *		shell is started.
 *
 *		If there is a shadow password file and the
 *		encrypted root password is "x" the shadow
 *		password will be used.
 *
 * Version:	@(#)sulogin 2.85-3 23-Apr-2003 miquels@cistron.nl
 *
 * Copyright (C) 1998-2003 Miquel van Smoorenburg.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 *
 */

#include <sys/types.h>
#include <sys/stat.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <pwd.h>
#include <shadow.h>
#include <termios.h>
#include <errno.h>
#include <sys/ioctl.h>
#ifdef HAVE_CRYPT_H
#  include <crypt.h>
#endif

#ifdef HAVE_LIBSELINUX
#  include <selinux/selinux.h>
#  include <selinux/get_context_list.h>
#endif

#include "c.h"
#include "nls.h"
#include "pathnames.h"

static int timeout;
static int profile;

struct sigaction saved_sigint;
struct sigaction saved_sigtstp;
struct sigaction saved_sigquit;

/*
 *	Called at timeout.
 */
static void alrm_handler(int sig __attribute__((unused)))
{
	return;
}

static void mask_signal(int signal, void (*handler)(int),
		struct sigaction *origaction)
{
	struct sigaction newaction;

	newaction.sa_handler = handler;
	sigemptyset(&newaction.sa_mask);
	newaction.sa_flags = 0;

	sigaction(signal, NULL, origaction);
	sigaction(signal, &newaction, NULL);
}

static void unmask_signal(int signal, struct sigaction *sa)
{
	sigaction(signal, sa, NULL);
}

/*
 *	See if an encrypted password is valid. The encrypted
 *	password is checked for traditional-style DES and
 *	FreeBSD-style MD5 encryption.
 */
static int valid(const char *pass)
{
	const char *s;
	char id[5];
	size_t len;
	off_t off;

	if (pass[0] == 0)
		return 1;
	if (pass[0] != '$')
		goto check_des;

	/*
	 *	up to 4 bytes for the signature e.g. $1$
	 */
	for (s = pass+1; *s && *s != '$'; s++);

	if (*s++ != '$')
		return 0;

	if ((off = (off_t)(s-pass)) > 4 || off < 3)
		return 0;

	memset(id, '\0', sizeof(id));
	strncpy(id, pass, off);

	/*
	 *	up to 16 bytes for the salt
	 */
	for (; *s && *s != '$'; s++);

	if (*s++ != '$')
		return 0;

	if ((off_t)(s-pass) > 16)
		return 0;

	len = strlen(s);

	/*
	 *	the MD5 hash (128 bits or 16 bytes) encoded in base64 = 22 bytes
	 */
	if ((strcmp(id, "$1$") == 0) && (len < 22 || len > 24))
		return 0;

	/*
	 *	the SHA-256 hash 43 bytes
	 */
	if ((strcmp(id, "$5$") == 0) && (len < 42 || len > 44))
		return 0;

	/*
	 *      the SHA-512 hash 86 bytes
	 */
	if ((strcmp(id, "$6$") == 0) && (len < 85 || len > 87))
		return 0;

	/*
	 *	e.g. Blowfish hash
	 */
	return 1;
check_des:
	if (strlen(pass) != 13)
		return 0;

	for (s = pass; *s; s++) {
		if ((*s < '0' || *s > '9') &&
		    (*s < 'a' || *s > 'z') &&
		    (*s < 'A' || *s > 'Z') &&
		    *s != '.' && *s != '/')
			return 0;
	}
	return 1;
}

/*
 *	Set a variable if the value is not NULL.
 */
static void set(char **var, char *val)
{
	if (val)
		*var = val;
}

/*
 *	Get the root password entry.
 */
static struct passwd *getrootpwent(int try_manually)
{
	static struct passwd pwd;
	struct passwd *pw;
	struct spwd *spw;
	FILE *fp;
	static char line[256];
	static char sline[256];
	char *p;

	/*
	 *	First, we try to get the password the standard
	 *	way using normal library calls.
	 */
	if ((pw = getpwnam("root")) &&
	    !strcmp(pw->pw_passwd, "x") &&
	    (spw = getspnam("root")))
		pw->pw_passwd = spw->sp_pwdp;
	if (pw || !try_manually)
		return pw;

	/*
	 *	If we come here, we could not retrieve the root
	 *	password through library calls and we try to
	 *	read the password and shadow files manually.
	 */
	pwd.pw_name = "root";
	pwd.pw_passwd = "";
	pwd.pw_gecos = "Super User";
	pwd.pw_dir = "/";
	pwd.pw_shell = "";
	pwd.pw_uid = 0;
	pwd.pw_gid = 0;

	if ((fp = fopen(_PATH_PASSWD, "r")) == NULL) {
		perror(_PATH_PASSWD);
		return &pwd;
	}

	/*
	 *	Find root in the password file.
	 */
	while ((p = fgets(line, 256, fp)) != NULL) {
		if (strncmp(line, "root:", 5) != 0)
			continue;
		p += 5;
		set(&pwd.pw_passwd, strsep(&p, ":"));
		strsep(&p, ":");
		strsep(&p, ":");
		set(&pwd.pw_gecos, strsep(&p, ":"));
		set(&pwd.pw_dir, strsep(&p, ":"));
		set(&pwd.pw_shell, strsep(&p, "\n"));
		p = line;
		break;
	}
	fclose(fp);

	/*
	 *	If the encrypted password is valid
	 *	or not found, return.
	 */
	if (p == NULL) {
		fprintf(stderr, "%s: no entry for root\n", _PATH_PASSWD);
		return &pwd;
	}
	if (valid(pwd.pw_passwd))
		return &pwd;

	/*
	 *	The password is invalid. If there is a
	 *	shadow password, try it.
	 */
	strcpy(pwd.pw_passwd, "");
	if ((fp = fopen(_PATH_SHADOW_PASSWD, "r")) == NULL) {
		fprintf(stderr, "%s: root password garbled\n", _PATH_PASSWD);
		return &pwd;
	}
	while ((p = fgets(sline, 256, fp)) != NULL) {
		if (strncmp(sline, "root:", 5) != 0)
			continue;
		p += 5;
		set(&pwd.pw_passwd, strsep(&p, ":"));
		break;
	}
	fclose(fp);

	/*
	 *	If the password is still invalid,
	 *	NULL it, and return.
	 */
	if (p == NULL) {
		fprintf(stderr, "%s: no entry for root\n", _PATH_SHADOW_PASSWD);
		strcpy(pwd.pw_passwd, "");
	}
	if (!valid(pwd.pw_passwd)) {
		fprintf(stderr, "%s: root password garbled\n", _PATH_SHADOW_PASSWD);
		strcpy(pwd.pw_passwd, "");
	}
	return &pwd;
}

/*
 *	Ask for the password. Note that there is no
 *	default timeout as we normally skip this during boot.
 */
static char *getpasswd(char *crypted)
{
	struct sigaction sa;
	struct termios old, tty;
	static char pass[128];
	char *ret = pass;
	size_t i;

	if (crypted[0])
		printf("Give root password for maintenance\n");
	else
		printf("Press enter for maintenance");
	printf("(or type Control-D to continue): ");
	fflush(stdout);

	tcgetattr(0, &old);
	tcgetattr(0, &tty);
	tty.c_iflag &= ~(IUCLC|IXON|IXOFF|IXANY);
	tty.c_lflag &= ~(ECHO|ECHOE|ECHOK|ECHONL|TOSTOP);
	tcsetattr(0, TCSANOW, &tty);

	pass[sizeof(pass) - 1] = 0;

	sa.sa_handler = alrm_handler;
	sa.sa_flags = 0;
	sigaction(SIGALRM, &sa, NULL);
	if (timeout)
		alarm(timeout);

	if (read(0, pass, sizeof(pass) - 1) <= 0)
		ret = NULL;
	else {
		for (i = 0; i < sizeof(pass) && pass[i]; i++)
			if (pass[i] == '\r' || pass[i] == '\n') {
				pass[i] = 0;
				break;
			}
	}
	alarm(0);
	tcsetattr(0, TCSANOW, &old);
	printf("\n");

	return ret;
}

/*
 *	Password was OK, execute a shell.
 */
static void sushell(struct passwd *pwd)
{
	char shell[128];
	char home[128];
	char *p;
	char *sushell;

	/*
	 *	Set directory and shell.
	 */
	chdir(pwd->pw_dir);
	if ((p = getenv("SUSHELL")) != NULL)
		sushell = p;
	else if ((p = getenv("sushell")) != NULL)
		sushell = p;
	else {
		if (pwd->pw_shell[0])
			sushell = pwd->pw_shell;
		else
			sushell = "/bin/sh";
	}
	if ((p = strrchr(sushell, '/')) == NULL)
		p = sushell;
	else
		p++;
	snprintf(shell, sizeof(shell), profile ? "-%s" : "%s", p);

	/*
	 *	Set some important environment variables.
	 */
	getcwd(home, sizeof(home));
	setenv("HOME", home, 1);
	setenv("LOGNAME", "root", 1);
	setenv("USER", "root", 1);
	if (!profile)
		setenv("SHLVL","0",1);

	/*
	 *	Try to execute a shell.
	 */
	setenv("SHELL", sushell, 1);
	unmask_signal(SIGINT, &saved_sigint);
	unmask_signal(SIGTSTP, &saved_sigtstp);
	unmask_signal(SIGQUIT, &saved_sigquit);
#ifdef WITH_SELINUX
	if (is_selinux_enabled() > 0) {
		security_context_t scon=NULL;
		char *seuser=NULL;
		char *level=NULL;
		if (getseuserbyname("root", &seuser, &level) == 0) {
			if (get_default_context_with_level(seuser, level, 0, &scon) == 0) {
				if (setexeccon(scon) != 0)
					fprintf(stderr, "setexeccon failed\n");
				freecon(scon);
			}
		}
		free(seuser);
		free(level);
	}
#endif
	execl(sushell, shell, NULL);
	perror(sushell);

	setenv("SHELL", "/bin/sh", 1);
	execl("/bin/sh", profile ? "-sh" : "sh", NULL);
	perror("/bin/sh");
}

static void usage(FILE *out)
{
	fprintf(out, USAGE_HEADER);
	fprintf(out, _(
			" %s [options] [tty device]\n"), program_invocation_short_name);

	fprintf(out, USAGE_OPTIONS);
	fprintf(out, _(
			" -p        start a login shell\n"
			" -t SEC    set max time to wait for a password (default: no limit)\n"
			" -e        examine shadow files directly if getpwnam(3) fails\n"
			" -h        display this help message\n"));

	fprintf(out, _("\nFor more information see sulogin(8).\n"));
}

int main(int argc, char **argv)
{
	char *tty = NULL;
	char *p;
	struct passwd *pwd;
	int c, fd = -1;
	int opt_e = 0;
	pid_t pid, pgrp, ppgrp, ttypgrp;
	struct sigaction saved_sighup;

	/*
	 *	See if we have a timeout flag.
	 */
	opterr = 0;
	while ((c = getopt(argc, argv, "ehpt:")) != EOF) {
		switch(c) {
		case 't':
			timeout = atoi(optarg);
			break;
		case 'p':
			profile = 1;
			break;
		case 'e':
			opt_e = 1;
			break;
		case 'h':
			usage(stdout);
			return 0;
		default:
			usage(stderr);
			/* Do not exit! */
			break;
		}
	}

	if (geteuid() != 0) {
		fprintf(stderr, "sulogin: only root can run sulogin.\n");
		exit(1);
	}

	/*
	 *	See if we need to open an other tty device.
	 */
	mask_signal(SIGQUIT, SIG_IGN, &saved_sigquit);
	mask_signal(SIGTSTP, SIG_IGN, &saved_sigtstp);
	mask_signal(SIGINT,  SIG_IGN, &saved_sigint);
	if (optind < argc)
		tty = argv[optind];

	if (tty || (tty = getenv("CONSOLE"))) {

		if ((fd = open(tty, O_RDWR)) < 0) {
			perror(tty);
			fd = dup(0);
		}

		if (!isatty(fd)) {
			fprintf(stderr, "%s: not a tty\n", tty);
			close(fd);
		} else {

			/*
			 *	Only go through this trouble if the new
			 *	tty doesn't fall in this process group.
			 */
			pid = getpid();
			pgrp = getpgid(0);
			ppgrp = getpgid(getppid());
			ttypgrp = tcgetpgrp(fd);

			if (pgrp != ttypgrp && ppgrp != ttypgrp) {
				if (pid != getsid(0)) {
					if (pid == getpgid(0))
						setpgid(0, getpgid(getppid()));
					setsid();
				}

				sigaction(SIGHUP, NULL, &saved_sighup);
				if (ttypgrp > 0)
					ioctl(0, TIOCNOTTY, (char *)1);
				sigaction(SIGHUP, &saved_sighup, NULL);
				close(0);
				close(1);
				close(2);
				if (fd > 2)
					close(fd);
				if ((fd = open(tty, O_RDWR|O_NOCTTY)) < 0) {
					perror(tty);
				} else {
					ioctl(0, TIOCSCTTY, (char *)1);
					tcsetpgrp(fd, ppgrp);
					dup2(fd, 0);
					dup2(fd, 1);
					dup2(fd, 2);
					if (fd > 2)
						close(fd);
				}
			} else
				if (fd > 2)
					close(fd);
		}
	} else if (getpid() == 1) {
		/* We are init. We hence need to set a session anyway */
		setsid();
		if (ioctl(0, TIOCSCTTY, (char *)1))
			perror("ioctl(TIOCSCTTY)");
	}

	/*
	 *	Get the root password.
	 */
	if ((pwd = getrootpwent(opt_e)) == NULL) {
		fprintf(stderr, "sulogin: cannot open password database!\n");
		sleep(2);
	}

	/*
	 *	Ask for the password.
	 */
	while (pwd) {
		if ((p = getpasswd(pwd->pw_passwd)) == NULL)
			break;
		if (pwd->pw_passwd[0] == 0 ||
		    strcmp(crypt(p, pwd->pw_passwd), pwd->pw_passwd) == 0)
			sushell(pwd);
		mask_signal(SIGQUIT, SIG_IGN, &saved_sigquit);
		mask_signal(SIGTSTP, SIG_IGN, &saved_sigtstp);
		mask_signal(SIGINT,  SIG_IGN, &saved_sigint);
		printf("Login incorrect.\n");
	}

	/*
	 *	User pressed Control-D.
	 */
	return 0;
}
