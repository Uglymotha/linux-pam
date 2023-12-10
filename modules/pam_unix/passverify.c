/*
 * Copyright information at end of file.
 */
#include "config.h"
#include <security/_pam_macros.h>
#include <security/pam_modules.h>
#include "support.h"
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>
#include <pwd.h>
#include <shadow.h>
#include <syslog.h>
#include <stdarg.h>
#include <signal.h>
#include <errno.h>
#include <time.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <fcntl.h>
#ifdef HAVE_CRYPT_H
#include <crypt.h>
#endif

#include "pam_cc_compat.h"
#include "pam_inline.h"
#include "md5.h"
#include "bigcrypt.h"
#include "passverify.h"

#ifdef WITH_SELINUX
#include <selinux/selinux.h>
#define SELINUX_ENABLED (is_selinux_enabled()>0)
#else
#define SELINUX_ENABLED 0
#endif

#ifdef HELPER_COMPILE
#define pam_modutil_getpwnam(h,n) getpwnam(n)
#define pam_modutil_getspnam(h,n) getspnam(n)
#define pam_syslog(h,a,b,c) helper_log_err(a,b,c)
#else
#include <security/pam_modutil.h>
#include <security/pam_ext.h>
#endif

#if defined(USE_LCKPWDF) && !defined(HAVE_LCKPWDF)
# include "./lckpwdf.-c"
#endif

static void
strip_hpux_aging(char *hash)
{
	static const char valid[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
		"abcdefghijklmnopqrstuvwxyz"
		"0123456789./";
	if ((*hash != '$') && (strlen(hash) > 13)) {
		for (hash += 13; *hash != '\0'; hash++) {
			if (strchr(valid, *hash) == NULL) {
				*hash = '\0';
				break;
			}
		}
	}
}

PAMH_ARG_DECL(int verify_pwd_hash,
	const char *p, char *hash, unsigned int nullok)
{
	size_t hash_len;
	char *pp = NULL;
	int retval;
	D(("called"));

	strip_hpux_aging(hash);
	hash_len = strlen(hash);
	if (!hash_len) {
		/* the stored password is NULL */
		if (nullok) { /* this means we've succeeded */
			D(("user has empty password - access granted"));
			retval = PAM_SUCCESS;
		} else {
			D(("user has empty password - access denied"));
			retval = PAM_AUTH_ERR;
		}
	} else if (!p || *hash == '*' || *hash == '!') {
		retval = PAM_AUTH_ERR;
	} else {
		if (pam_str_skip_prefix(hash, "$1$") != NULL) {
			pp = Goodcrypt_md5(p, hash);
			if (pp && strcmp(pp, hash) != 0) {
				_pam_delete(pp);
				pp = Brokencrypt_md5(p, hash);
			}
		} else if (*hash != '$' && hash_len >= 13) {
			pp = bigcrypt(p, hash);
			if (pp && hash_len == 13 && strlen(pp) > hash_len) {
				pam_overwrite_string(pp + hash_len);
			}
		} else {
			/*
			 * Ok, we don't know the crypt algorithm, but maybe
			 * libcrypt knows about it? We should try it.
			 */
#if defined(CRYPT_CHECKSALT_AVAILABLE) && CRYPT_CHECKSALT_AVAILABLE
			/* Get the status of the hash from checksalt */
			int retval_checksalt = crypt_checksalt(hash);

			/*
			 * Check for hashing methods that are disabled by
			 * libcrypt configuration and/or system preset.
			 */
			if (retval_checksalt == CRYPT_SALT_METHOD_DISABLED) {
				/*
				 * pam_syslog() needs a pam handle,
				 * but that's not available here.
				 */
				pam_syslog(pamh, LOG_ERR,
				  "The support for password hash \"%.6s\" "
				  "has been disabled in libcrypt "
				  "configuration.",
				  hash);
			}
			/*
			 * Check for malformed hashes, like descrypt hashes
			 * starting with "$2...", which might have been
			 * generated by unsafe base64 encoding functions
			 * as used in glibc <= 2.16.
			 * Such hashes are likely to be rejected by many
			 * recent implementations of libcrypt.
			 */
			if (retval_checksalt == CRYPT_SALT_INVALID) {
				pam_syslog(pamh, LOG_ERR,
				  "The password hash \"%.6s\" is unknown to "
				  "libcrypt.",
				  hash);
			}
#else
#ifndef HELPER_COMPILE
			(void)pamh;
#endif
#endif
#ifdef HAVE_CRYPT_R
			struct crypt_data *cdata;
			cdata = malloc(sizeof(*cdata));
			if (cdata != NULL) {
				cdata->initialized = 0;
				pp = x_strdup(crypt_r(p, hash, cdata));
				pam_overwrite_object(cdata);
				free(cdata);
			}
#else
			pp = x_strdup(crypt(p, hash));
#endif
		}
		p = NULL;		/* no longer needed here */

		/* the moment of truth -- do we agree with the password? */
		D(("comparing state of pp[%s] and hash[%s]", pp ? pp : "(null)", hash));

		if (pp && strcmp(pp, hash) == 0) {
			retval = PAM_SUCCESS;
		} else {
			retval = PAM_AUTH_ERR;
		}
	}

	if (pp)
		_pam_delete(pp);
	D(("done [%d].", retval));

	return retval;
}

int
is_pwd_shadowed(const struct passwd *pwd)
{
	if (pwd != NULL) {
		if (strcmp(pwd->pw_passwd, "x") == 0) {
			return 1;
		}
		if ((pwd->pw_passwd[0] == '#') &&
		    (pwd->pw_passwd[1] == '#') &&
		    (strcmp(pwd->pw_name, pwd->pw_passwd + 2) == 0)) {
			return 1;
		}
	}
	return 0;
}

PAMH_ARG_DECL(int get_account_info,
	const char *name, struct passwd **pwd, struct spwd **spwdent)
{
	/* UNIX passwords area */
	*pwd = pam_modutil_getpwnam(pamh, name);	/* Get password file entry... */
	*spwdent = NULL;

	if (*pwd != NULL) {
		if (strcmp((*pwd)->pw_passwd, "*NP*") == 0)
		{ /* NIS+ */
#ifdef HELPER_COMPILE
			uid_t save_euid, save_uid;

			save_euid = geteuid();
			save_uid = getuid();
			if (save_uid == (*pwd)->pw_uid) {
				if (setreuid(save_euid, save_uid))
					return PAM_CRED_INSUFFICIENT;
			} else  {
				if (setreuid(0, -1))
					return PAM_CRED_INSUFFICIENT;
				if (setreuid(-1, (*pwd)->pw_uid)) {
					if (setreuid(-1, 0)
					    || setreuid(0, -1)
					    || setreuid(-1, (*pwd)->pw_uid)) {
						return PAM_CRED_INSUFFICIENT;
					}
				}
			}

			*spwdent = pam_modutil_getspnam(pamh, name);
			if (save_uid == (*pwd)->pw_uid) {
				if (setreuid(save_uid, save_euid))
					return PAM_CRED_INSUFFICIENT;
			} else {
				if (setreuid(-1, 0)
				    || setreuid(save_uid, -1)
				    || setreuid(-1, save_euid))
					return PAM_CRED_INSUFFICIENT;
			}

			if (*spwdent == NULL || (*spwdent)->sp_pwdp == NULL)
				return PAM_AUTHINFO_UNAVAIL;
#else
			/* we must run helper for NIS+ passwords */
			return PAM_UNIX_RUN_HELPER;
#endif
		} else if (is_pwd_shadowed(*pwd)) {
			/*
			 * ...and shadow password file entry for this user,
			 * if shadowing is enabled
			 */
			*spwdent = pam_modutil_getspnam(pamh, name);
			if (*spwdent == NULL) {
#ifndef HELPER_COMPILE
				/* still a chance the user can authenticate */
				return PAM_UNIX_RUN_HELPER;
#endif
				return PAM_AUTHINFO_UNAVAIL;
			}
			if ((*spwdent)->sp_pwdp == NULL)
				return PAM_AUTHINFO_UNAVAIL;
		}
	} else {
		return PAM_USER_UNKNOWN;
	}
	return PAM_SUCCESS;
}

PAMH_ARG_DECL(int get_pwd_hash,
	const char *name, struct passwd **pwd, char **hash)
{
	int retval;
	struct spwd *spwdent = NULL;

	retval = get_account_info(PAMH_ARG(name, pwd, &spwdent));
	if (retval != PAM_SUCCESS) {
		return retval;
	}

	if (spwdent)
		*hash = x_strdup(spwdent->sp_pwdp);
	else
		*hash = x_strdup((*pwd)->pw_passwd);
	if (*hash == NULL)
		return PAM_BUF_ERR;

	return PAM_SUCCESS;
}

PAMH_ARG_DECL(int check_shadow_expiry,
	struct spwd *spent, int *daysleft)
{
	long int curdays;
	*daysleft = -1;
	curdays = (long int)(time(NULL) / (60 * 60 * 24));
	D(("today is %ld, last change %ld", curdays, spent->sp_lstchg));
	if ((curdays >= spent->sp_expire) && (spent->sp_expire != -1)) {
		D(("account expired"));
		return PAM_ACCT_EXPIRED;
	}
	if (spent->sp_lstchg == 0) {
		D(("need a new password"));
		*daysleft = 0;
		return PAM_NEW_AUTHTOK_REQD;
	}
	if (curdays < spent->sp_lstchg) {
		pam_syslog(pamh, LOG_DEBUG,
			 "account %s has password changed in future",
			 spent->sp_namp);
		return PAM_SUCCESS;
	}
	if ((curdays - spent->sp_lstchg > spent->sp_max)
	    && (curdays - spent->sp_lstchg > spent->sp_inact)
	    && (curdays - spent->sp_lstchg > spent->sp_max + spent->sp_inact)
	    && (spent->sp_max != -1) && (spent->sp_inact != -1)) {
		*daysleft = (int)((spent->sp_lstchg + spent->sp_max) - curdays);
		D(("authtok expired"));
		return PAM_AUTHTOK_EXPIRED;
	}
	if ((curdays - spent->sp_lstchg > spent->sp_max) && (spent->sp_max != -1)) {
		D(("need a new password 2"));
		return PAM_NEW_AUTHTOK_REQD;
	}
	if ((curdays - spent->sp_lstchg > spent->sp_max - spent->sp_warn)
	    && (spent->sp_max != -1) && (spent->sp_warn != -1)) {
		*daysleft = (int)((spent->sp_lstchg + spent->sp_max) - curdays);
		D(("warn before expiry"));
	}
	if ((curdays - spent->sp_lstchg < spent->sp_min)
	    && (spent->sp_min != -1)) {
		/*
		 * The last password change was too recent. This error will be ignored
		 * if no password change is attempted.
		 */
		D(("password change too recent"));
		return PAM_AUTHTOK_ERR;
	}
	return PAM_SUCCESS;
}

/* passwd/salt conversion macros */

#define PW_TMPFILE              "/etc/npasswd"
#define SH_TMPFILE              "/etc/nshadow"
#define OPW_TMPFILE             SCONFIGDIR "/nopasswd"

/*
 * i64c - convert an integer to a radix 64 character
 */
static int
i64c(int i)
{
        if (i < 0)
                return ('.');
        else if (i > 63)
                return ('z');
        if (i == 0)
                return ('.');
        if (i == 1)
                return ('/');
        if (i >= 2 && i <= 11)
                return ('0' - 2 + i);
        if (i >= 12 && i <= 37)
                return ('A' - 12 + i);
        if (i >= 38 && i <= 63)
                return ('a' - 38 + i);
        return ('\0');
}

/* <where> must point to a buffer of at least <length>+1 length */
static void
crypt_make_salt(char *where, int length)
{
        struct timeval tv;
        MD5_CTX ctx;
        unsigned char tmp[16];
        unsigned char *src = (unsigned char *)where;
        int i;
#ifdef PAM_PATH_RANDOMDEV
	int fd;
	int rv;

	if ((rv = fd = open(PAM_PATH_RANDOMDEV, O_RDONLY)) != -1) {
		while ((rv = read(fd, where, length)) != length && errno == EINTR);
		close (fd);
	}
	if (rv != length) {
#endif
        /*
         * Code lifted from Marek Michalkiewicz's shadow suite. (CG)
         * removed use of static variables (AGM)
         *
	 * will work correctly only for length <= 16 */
	src = tmp;
        GoodMD5Init(&ctx);
        gettimeofday(&tv, (struct timezone *) 0);
        GoodMD5Update(&ctx, (void *) &tv, sizeof tv);
        i = getpid();
        GoodMD5Update(&ctx, (void *) &i, sizeof i);
        i = clock();
        GoodMD5Update(&ctx, (void *) &i, sizeof i);
        GoodMD5Update(&ctx, src, length);
        GoodMD5Final(tmp, &ctx);
#ifdef PAM_PATH_RANDOMDEV
	}
#endif
        for (i = 0; i < length; i++)
                *where++ = i64c(src[i] & 077);
        *where = '\0';
}

char *
crypt_md5_wrapper(const char *pass_new)
{
        unsigned char result[16];
        char *cp = (char *) result;

        cp = stpcpy(cp, "$1$");      /* magic for the MD5 */
	crypt_make_salt(cp, 8);

        /* no longer need cleartext */
        cp = Goodcrypt_md5(pass_new, (const char *) result);
	pass_new = NULL;

        return cp;
}

PAMH_ARG_DECL(char * create_password_hash,
	const char *password, unsigned long long ctrl, int rounds)
{
	const char *algoid;
#if defined(CRYPT_GENSALT_OUTPUT_SIZE) && CRYPT_GENSALT_OUTPUT_SIZE > 64
	/* Strings returned by crypt_gensalt_rn will be no longer than this. */
	char salt[CRYPT_GENSALT_OUTPUT_SIZE];
#else
	char salt[64]; /* contains rounds number + max 16 bytes of salt + algo id */
#endif
	char *sp, *ret;
#ifdef HAVE_CRYPT_R
	struct crypt_data *cdata = NULL;
#endif

	if (on(UNIX_MD5_PASS, ctrl)) {
		/* algoid = "$1" */
		return crypt_md5_wrapper(password);
	} else if (on(UNIX_YESCRYPT_PASS, ctrl)) {
		algoid = "$y$";
	} else if (on(UNIX_GOST_YESCRYPT_PASS, ctrl)) {
		algoid = "$gy$";
	} else if (on(UNIX_BLOWFISH_PASS, ctrl)) {
		algoid = "$2b$";
	} else if (on(UNIX_SHA256_PASS, ctrl)) {
		algoid = "$5$";
	} else if (on(UNIX_SHA512_PASS, ctrl)) {
		algoid = "$6$";
	} else { /* must be crypt/bigcrypt */
		char tmppass[9];
		char *hashed;

		crypt_make_salt(salt, 2);
		if (off(UNIX_BIGCRYPT, ctrl) && strlen(password) > 8) {
			strncpy(tmppass, password, sizeof(tmppass)-1);
			tmppass[sizeof(tmppass)-1] = '\0';
			password = tmppass;
		}
		hashed = bigcrypt(password, salt);
		pam_overwrite_array(tmppass);
		password = NULL;
		return hashed;
	}

#if defined(CRYPT_GENSALT_IMPLEMENTS_AUTO_ENTROPY) && CRYPT_GENSALT_IMPLEMENTS_AUTO_ENTROPY
	/*
	 * Any version of libcrypt supporting auto entropy is
	 * guaranteed to have crypt_gensalt_rn().
	 */
	sp = crypt_gensalt_rn(algoid, rounds, NULL, 0, salt, sizeof(salt));
#else
	sp = stpcpy(salt, algoid);
	if (on(UNIX_ALGO_ROUNDS, ctrl)) {
		sp += snprintf(sp, sizeof(salt) - (16 + 1 + (sp - salt)), "rounds=%u$", rounds);
	}
	crypt_make_salt(sp, 16);
#endif /* CRYPT_GENSALT_IMPLEMENTS_AUTO_ENTROPY */
#ifdef HAVE_CRYPT_R
	sp = NULL;
	cdata = malloc(sizeof(*cdata));
	if (cdata != NULL) {
		cdata->initialized = 0;
		sp = crypt_r(password, salt, cdata);
	}
#else
	sp = crypt(password, salt);
#endif
	if (!sp || strncmp(algoid, sp, strlen(algoid)) != 0) {
		/* libxcrypt/libc doesn't know the algorithm, error out */
		pam_syslog(pamh, LOG_ERR,
			   "Algo %s not supported by the crypto backend.\n",
			   on(UNIX_YESCRYPT_PASS, ctrl) ? "yescrypt" :
			   on(UNIX_GOST_YESCRYPT_PASS, ctrl) ? "gost_yescrypt" :
			   on(UNIX_BLOWFISH_PASS, ctrl) ? "blowfish" :
			   on(UNIX_SHA256_PASS, ctrl) ? "sha256" :
			   on(UNIX_SHA512_PASS, ctrl) ? "sha512" : algoid);
		if(sp) {
		   pam_overwrite_string(sp);
		}
#ifdef HAVE_CRYPT_R
		pam_overwrite_object(cdata);
		free(cdata);
#endif
		return NULL;
	}
	ret = strdup(sp);
	pam_overwrite_string(sp);
#ifdef HAVE_CRYPT_R
	pam_overwrite_object(cdata);
	free(cdata);
#endif
	return ret;
}

#ifdef WITH_SELINUX
int
unix_selinux_confined(void)
{
    static int confined = -1;
    int fd;
    char tempfile[]="/etc/.pwdXXXXXX";

    if (confined != -1)
	return confined;

    /* cannot be confined without SELinux enabled */
    if (!SELINUX_ENABLED){
	confined = 0;
	return confined;
    }

    /* let's try opening shadow read only */
    if ((fd=open("/etc/shadow", O_RDONLY)) != -1) {
        close(fd);
        confined = 0;
        return confined;
    }

    if (errno == EACCES) {
	confined = 1;
	return confined;
    }

    /* shadow opening failed because of other reasons let's try
       creating a file in /etc */
    if ((fd=mkstemp(tempfile)) != -1) {
        unlink(tempfile);
        close(fd);
        confined = 0;
        return confined;
    }

    confined = 1;
    return confined;
}

#else
int
unix_selinux_confined(void)
{
    return 0;
}
#endif

#ifdef USE_LCKPWDF
int
lock_pwdf(void)
{
        int i;
        int retval;

#ifndef HELPER_COMPILE
        if (unix_selinux_confined()) {
                return PAM_SUCCESS;
        }
#endif
        /* These values for the number of attempts and the sleep time
           are, of course, completely arbitrary.
           My reading of the PAM docs is that, once pam_chauthtok() has been
           called with PAM_UPDATE_AUTHTOK, we are obliged to take any
           reasonable steps to make sure the token is updated; so retrying
           for 1/10 sec. isn't overdoing it. */
        i=0;
        while((retval = lckpwdf()) != 0 && i < 100) {
                usleep(1000);
                i++;
        }
        if(retval != 0) {
                return PAM_AUTHTOK_LOCK_BUSY;
        }
        return PAM_SUCCESS;
}

void
unlock_pwdf(void)
{
#ifndef HELPER_COMPILE
        if (unix_selinux_confined()) {
                return;
        }
#endif
        ulckpwdf();
}
#else
int
lock_pwdf(void)
{
	return PAM_SUCCESS;
}

void
unlock_pwdf(void)
{
	return;
}
#endif

#ifdef HELPER_COMPILE
int
save_old_password(const char *forwho, const char *oldpass,
		  int howmany)
#else
int
save_old_password(pam_handle_t *pamh, const char *forwho, const char *oldpass,
		  int howmany)
#endif
{
    static char buf[16384];
    static char nbuf[16384];
    char *s_luser, *s_uid, *s_npas, *s_pas, *pass;
    int npas;
    FILE *pwfile, *opwfile;
    int err = 0;
    int oldmask;
    int found = 0;
    struct passwd *pwd = NULL;
    struct stat st;
    size_t len = strlen(forwho);
#ifdef WITH_SELINUX
    char *prev_context_raw = NULL;
#endif

    if (howmany < 0) {
	return PAM_SUCCESS;
    }

    if (oldpass == NULL) {
	return PAM_SUCCESS;
    }

    oldmask = umask(077);

#ifdef WITH_SELINUX
    if (SELINUX_ENABLED) {
      char *passwd_context_raw = NULL;
      if (getfilecon_raw("/etc/passwd",&passwd_context_raw)<0) {
        return PAM_AUTHTOK_ERR;
      };
      if (getfscreatecon_raw(&prev_context_raw)<0) {
        freecon(passwd_context_raw);
        return PAM_AUTHTOK_ERR;
      }
      if (setfscreatecon_raw(passwd_context_raw)) {
        freecon(passwd_context_raw);
        freecon(prev_context_raw);
        return PAM_AUTHTOK_ERR;
      }
      freecon(passwd_context_raw);
    }
#endif
    pwfile = fopen(OPW_TMPFILE, "w");
    umask(oldmask);
    if (pwfile == NULL) {
      err = 1;
      goto done;
    }

    opwfile = fopen(OLD_PASSWORDS_FILE, "r");
    if (opwfile == NULL) {
	fclose(pwfile);
      err = 1;
      goto done;
    }

    if (fstat(fileno(opwfile), &st) == -1) {
	fclose(opwfile);
	fclose(pwfile);
	err = 1;
	goto done;
    }

    if (fchown(fileno(pwfile), st.st_uid, st.st_gid) == -1) {
	fclose(opwfile);
	fclose(pwfile);
	err = 1;
	goto done;
    }
    if (fchmod(fileno(pwfile), st.st_mode) == -1) {
	fclose(opwfile);
	fclose(pwfile);
	err = 1;
	goto done;
    }

    while (fgets(buf, 16380, opwfile)) {
	if (!strncmp(buf, forwho, len) && strchr(":,\n", buf[len]) != NULL) {
	    char *ep, *sptr = NULL;
	    long value;
	    found = 1;
	    if (howmany == 0)
		continue;
	    buf[strlen(buf) - 1] = '\0';
	    s_luser = strtok_r(buf, ":", &sptr);
	    if (s_luser == NULL) {
		found = 0;
		continue;
	    }
	    s_uid = strtok_r(NULL, ":", &sptr);
	    if (s_uid == NULL) {
		found = 0;
		continue;
	    }
	    s_npas = strtok_r(NULL, ":", &sptr);
	    if (s_npas == NULL) {
		found = 0;
		continue;
	    }
	    s_pas = strtok_r(NULL, ":", &sptr);
	    value = strtol(s_npas, &ep, 10);
	    if (value < 0 || value >= INT_MAX || s_npas == ep || *ep != '\0')
		npas = 0;
	    else
		npas = (int)value + 1;
	    while (npas > howmany && s_pas != NULL) {
		s_pas = strpbrk(s_pas, ",");
		if (s_pas != NULL)
		    s_pas++;
		npas--;
	    }
	    pass = crypt_md5_wrapper(oldpass);
	    if (s_pas == NULL)
		snprintf(nbuf, sizeof(nbuf), "%s:%s:%d:%s\n",
			 s_luser, s_uid, npas, pass);
	    else
		snprintf(nbuf, sizeof(nbuf),"%s:%s:%d:%s,%s\n",
			 s_luser, s_uid, npas, s_pas, pass);
	    _pam_delete(pass);
	    if (fputs(nbuf, pwfile) < 0) {
		err = 1;
		break;
	    }
	} else if (fputs(buf, pwfile) < 0) {
	    err = 1;
	    break;
	}
    }
    fclose(opwfile);

    if (!found) {
	pwd = pam_modutil_getpwnam(pamh, forwho);
	if (pwd == NULL) {
	    err = 1;
	} else {
	    pass = crypt_md5_wrapper(oldpass);
	    snprintf(nbuf, sizeof(nbuf), "%s:%lu:1:%s\n",
		     forwho, (unsigned long)pwd->pw_uid, pass);
	    _pam_delete(pass);
	    if (fputs(nbuf, pwfile) < 0) {
		err = 1;
	    }
	}
    }

    if (fflush(pwfile) || fsync(fileno(pwfile))) {
	D(("fflush or fsync error writing entries to old passwords file: %m"));
	err = 1;
    }

    if (fclose(pwfile)) {
	D(("fclose error writing entries to old passwords file: %m"));
	err = 1;
    }

done:
    if (!err) {
	if (rename(OPW_TMPFILE, OLD_PASSWORDS_FILE))
	    err = 1;
    }
#ifdef WITH_SELINUX
    if (SELINUX_ENABLED) {
      if (setfscreatecon_raw(prev_context_raw)) {
        err = 1;
      }
      if (prev_context_raw)
        freecon(prev_context_raw);
      prev_context_raw = NULL;
    }
#endif
    if (!err) {
	return PAM_SUCCESS;
    } else {
	unlink(OPW_TMPFILE);
	return PAM_AUTHTOK_ERR;
    }
}

PAMH_ARG_DECL(int unix_update_passwd,
	const char *forwho, const char *towhat)
{
    struct passwd *tmpent = NULL;
    struct stat st;
    FILE *pwfile, *opwfile;
    int err = 1;
    int oldmask;
#ifdef WITH_SELINUX
    char *prev_context_raw = NULL;
#endif

    oldmask = umask(077);
#ifdef WITH_SELINUX
    if (SELINUX_ENABLED) {
      char *passwd_context_raw = NULL;
      if (getfilecon_raw("/etc/passwd",&passwd_context_raw)<0) {
	return PAM_AUTHTOK_ERR;
      };
      if (getfscreatecon_raw(&prev_context_raw)<0) {
	freecon(passwd_context_raw);
	return PAM_AUTHTOK_ERR;
      }
      if (setfscreatecon_raw(passwd_context_raw)) {
	freecon(passwd_context_raw);
	freecon(prev_context_raw);
	return PAM_AUTHTOK_ERR;
      }
      freecon(passwd_context_raw);
    }
#endif
    pwfile = fopen(PW_TMPFILE, "w");
    umask(oldmask);
    if (pwfile == NULL) {
      err = 1;
      goto done;
    }

    opwfile = fopen("/etc/passwd", "r");
    if (opwfile == NULL) {
	fclose(pwfile);
	err = 1;
	goto done;
    }

    if (fstat(fileno(opwfile), &st) == -1) {
	fclose(opwfile);
	fclose(pwfile);
	err = 1;
	goto done;
    }

    if (fchown(fileno(pwfile), st.st_uid, st.st_gid) == -1) {
	fclose(opwfile);
	fclose(pwfile);
	err = 1;
	goto done;
    }
    if (fchmod(fileno(pwfile), st.st_mode) == -1) {
	fclose(opwfile);
	fclose(pwfile);
	err = 1;
	goto done;
    }

    tmpent = fgetpwent(opwfile);
    while (tmpent) {
	if (!strcmp(tmpent->pw_name, forwho)) {
	    /* To shut gcc up */
	    union {
		const char *const_charp;
		char *charp;
	    } assigned_passwd;
	    assigned_passwd.const_charp = towhat;

	    tmpent->pw_passwd = assigned_passwd.charp;
	    err = 0;
	}
	if (putpwent(tmpent, pwfile)) {
	    D(("error writing entry to password file: %m"));
	    err = 1;
	    break;
	}
	tmpent = fgetpwent(opwfile);
    }
    fclose(opwfile);

    if (fflush(pwfile) || fsync(fileno(pwfile))) {
	D(("fflush or fsync error writing entries to password file: %m"));
	err = 1;
    }

    if (fclose(pwfile)) {
	D(("fclose error writing entries to password file: %m"));
	err = 1;
    }

done:
    if (!err) {
	if (!rename(PW_TMPFILE, "/etc/passwd"))
	    pam_syslog(pamh,
		LOG_NOTICE, "password changed for %s", forwho);
	else
	    err = 1;
    }
#ifdef WITH_SELINUX
    if (SELINUX_ENABLED) {
      if (setfscreatecon_raw(prev_context_raw)) {
	err = 1;
      }
      if (prev_context_raw)
	freecon(prev_context_raw);
      prev_context_raw = NULL;
    }
#endif
    if (!err) {
	return PAM_SUCCESS;
    } else {
	unlink(PW_TMPFILE);
	return PAM_AUTHTOK_ERR;
    }
}

PAMH_ARG_DECL(int unix_update_shadow,
	const char *forwho, char *towhat)
{
    struct spwd spwdent, *stmpent = NULL;
    struct stat st;
    FILE *pwfile, *opwfile;
    int err = 0;
    int oldmask;
    int wroteentry = 0;
#ifdef WITH_SELINUX
    char *prev_context_raw = NULL;
#endif

    oldmask = umask(077);

#ifdef WITH_SELINUX
    if (SELINUX_ENABLED) {
      char *shadow_context_raw = NULL;
      if (getfilecon_raw("/etc/shadow",&shadow_context_raw)<0) {
	return PAM_AUTHTOK_ERR;
      };
      if (getfscreatecon_raw(&prev_context_raw)<0) {
	freecon(shadow_context_raw);
	return PAM_AUTHTOK_ERR;
      }
      if (setfscreatecon_raw(shadow_context_raw)) {
	freecon(shadow_context_raw);
	freecon(prev_context_raw);
	return PAM_AUTHTOK_ERR;
      }
      freecon(shadow_context_raw);
    }
#endif
    pwfile = fopen(SH_TMPFILE, "w");
    umask(oldmask);
    if (pwfile == NULL) {
	err = 1;
	goto done;
    }

    opwfile = fopen("/etc/shadow", "r");
    if (opwfile == NULL) {
	fclose(pwfile);
	err = 1;
	goto done;
    }

    if (fstat(fileno(opwfile), &st) == -1) {
	fclose(opwfile);
	fclose(pwfile);
	err = 1;
	goto done;
    }

    if (fchown(fileno(pwfile), st.st_uid, st.st_gid) == -1) {
	fclose(opwfile);
	fclose(pwfile);
	err = 1;
	goto done;
    }
    if (fchmod(fileno(pwfile), st.st_mode) == -1) {
	fclose(opwfile);
	fclose(pwfile);
	err = 1;
	goto done;
    }

    stmpent = fgetspent(opwfile);
    while (stmpent) {

	if (!strcmp(stmpent->sp_namp, forwho)) {
	    stmpent->sp_pwdp = towhat;
	    stmpent->sp_lstchg = time(NULL) / (60 * 60 * 24);
	    if (stmpent->sp_lstchg == 0)
	        stmpent->sp_lstchg = -1; /* Don't request passwort change
					    only because time isn't set yet. */
	    wroteentry = 1;
	    D(("Set password %s for %s", stmpent->sp_pwdp, forwho));
	}

	if (putspent(stmpent, pwfile)) {
	    D(("error writing entry to shadow file: %m"));
	    err = 1;
	    break;
	}

	stmpent = fgetspent(opwfile);
    }

    fclose(opwfile);

    if (!wroteentry && !err) {
	DIAG_PUSH_IGNORE_CAST_QUAL;
	spwdent.sp_namp = (char *)forwho;
	DIAG_POP_IGNORE_CAST_QUAL;
	spwdent.sp_pwdp = towhat;
	spwdent.sp_lstchg = time(NULL) / (60 * 60 * 24);
	if (spwdent.sp_lstchg == 0)
	    spwdent.sp_lstchg = -1; /* Don't request passwort change
				       only because time isn't set yet. */
	spwdent.sp_min = spwdent.sp_max = spwdent.sp_warn = spwdent.sp_inact =
	    spwdent.sp_expire = -1;
	spwdent.sp_flag = (unsigned long)-1l;
	if (putspent(&spwdent, pwfile)) {
	    D(("error writing entry to shadow file: %m"));
	    err = 1;
	}
    }

    if (fflush(pwfile) || fsync(fileno(pwfile))) {
	D(("fflush or fsync error writing entries to shadow file: %m"));
	err = 1;
    }

    if (fclose(pwfile)) {
	D(("fclose error writing entries to shadow file: %m"));
	err = 1;
    }

 done:
    if (!err) {
	if (!rename(SH_TMPFILE, "/etc/shadow"))
	    pam_syslog(pamh,
		LOG_NOTICE, "password changed for %s", forwho);
	else
	    err = 1;
    }

#ifdef WITH_SELINUX
    if (SELINUX_ENABLED) {
      if (setfscreatecon_raw(prev_context_raw)) {
	err = 1;
      }
      if (prev_context_raw)
	freecon(prev_context_raw);
      prev_context_raw = NULL;
    }
#endif

    if (!err) {
	return PAM_SUCCESS;
    } else {
	unlink(SH_TMPFILE);
	return PAM_AUTHTOK_ERR;
    }
}

#ifdef HELPER_COMPILE

int
helper_verify_password(const char *name, const char *p, int nullok)
{
	struct passwd *pwd = NULL;
	char *hash = NULL;
	int retval;

	retval = get_pwd_hash(name, &pwd, &hash);

	if (pwd == NULL || hash == NULL) {
		helper_log_err(LOG_NOTICE, "check pass; user unknown");
		retval = PAM_USER_UNKNOWN;
	} else if (p[0] == '\0' && nullok) {
		if (hash[0] == '\0') {
			retval = PAM_SUCCESS;
		} else {
			retval = PAM_AUTH_ERR;
		}
	} else {
		retval = verify_pwd_hash(p, hash, nullok);
	}

	if (hash) {
		pam_overwrite_string(hash);
		_pam_drop(hash);
	}

	p = NULL;		/* no longer needed here */

	return retval;
}

void
PAM_FORMAT((printf, 2, 3))
helper_log_err(int err, const char *format, ...)
{
	va_list args;

	va_start(args, format);
	openlog(HELPER_COMPILE, LOG_CONS | LOG_PID, LOG_AUTHPRIV);
	vsyslog(err, format, args);
	va_end(args);
	closelog();
}

static void
su_sighandler(int sig)
{
#ifndef SA_RESETHAND
        /* emulate the behaviour of the SA_RESETHAND flag */
        if ( sig == SIGILL || sig == SIGTRAP || sig == SIGBUS || sig = SIGSERV ) {
		struct sigaction sa;
		memset(&sa, '\0', sizeof(sa));
		sa.sa_handler = SIG_DFL;
                sigaction(sig, &sa, NULL);
	}
#endif
        if (sig > 0) {
                _exit(sig);
        }
}

void
setup_signals(void)
{
        struct sigaction action;        /* posix signal structure */

        /*
         * Setup signal handlers
         */
        (void) memset((void *) &action, 0, sizeof(action));
        action.sa_handler = su_sighandler;
#ifdef SA_RESETHAND
        action.sa_flags = SA_RESETHAND;
#endif
        (void) sigaction(SIGILL, &action, NULL);
        (void) sigaction(SIGTRAP, &action, NULL);
        (void) sigaction(SIGBUS, &action, NULL);
        (void) sigaction(SIGSEGV, &action, NULL);
        action.sa_handler = SIG_IGN;
        action.sa_flags = 0;
        (void) sigaction(SIGTERM, &action, NULL);
        (void) sigaction(SIGHUP, &action, NULL);
        (void) sigaction(SIGINT, &action, NULL);
        (void) sigaction(SIGQUIT, &action, NULL);
}

char *
getuidname(uid_t uid)
{
        struct passwd *pw;
        static char username[256];

        pw = getpwuid(uid);
        if (pw == NULL)
                return NULL;

        strncpy(username, pw->pw_name, sizeof(username));
        username[sizeof(username) - 1] = '\0';

        return username;
}

#endif
/* ****************************************************************** *
 * Copyright (c) Jan Rękorajski 1999.
 * Copyright (c) Andrew G. Morgan 1996-8.
 * Copyright (c) Alex O. Yuriev, 1996.
 * Copyright (c) Cristian Gafton 1996.
 * Copyright (c) Red Hat, Inc. 1996, 2007, 2008.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, and the entire permission notice in its entirety,
 *    including the disclaimer of warranties.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. The name of the author may not be used to endorse or promote
 *    products derived from this software without specific prior
 *    written permission.
 *
 * ALTERNATIVELY, this product may be distributed under the terms of
 * the GNU Public License, in which case the provisions of the GPL are
 * required INSTEAD OF the above restrictions.  (This clause is
 * necessary due to a potential bad interaction between the GPL and
 * the restrictions contained in a BSD-style copyright.)
 *
 * THIS SOFTWARE IS PROVIDED ``AS IS'' AND ANY EXPRESS OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT,
 * INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
 * STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED
 * OF THE POSSIBILITY OF SUCH DAMAGE.
 */
