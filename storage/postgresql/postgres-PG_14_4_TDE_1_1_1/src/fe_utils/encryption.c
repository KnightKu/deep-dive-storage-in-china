/*-------------------------------------------------------------------------
 *
 * encryption.c
 *	  Front-end code to handle keys for full cluster encryption. The actual
 *	  encryption is not performed here.
 *
 * Portions Copyright (c) 2019-2022, CYBERTEC PostgreSQL International GmbH
 * Portions Copyright (c) 1996-2022, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/fe_utils/encryption.c
 *
 *-------------------------------------------------------------------------
 */

#include <fcntl.h>
#include <unistd.h>
#include <sys/time.h>
#include <sys/wait.h>

#include "postgres_fe.h"

#include "common/fe_memutils.h"
#include "common/file_perm.h"
#include "common/logging.h"
#include "fe_utils/encryption.h"
#include "libpq-fe.h"
#include "libpq/pqcomm.h"

#ifdef USE_ENCRYPTION
#include <openssl/evp.h>

/*
 * Key derivation function.
 */
typedef enum KDFKind
{
	KDF_OPENSSL_PKCS5_PBKDF2_HMAC_SHA = 0
} KFDKind;

typedef struct KDFParamsPBKDF2
{
	unsigned long int niter;
	unsigned char salt[ENCRYPTION_KDF_SALT_LEN];
} KDFParamsPBKDF2;


typedef struct KDFParamsData_v0
{
	KFDKind		function;

	/*
	 * Function-specific parameters.
	 */
	union
	{
		KDFParamsPBKDF2 pbkdf2;
	}			data;

	/* CRC of all above ... MUST BE LAST! */
	pg_crc32c	crc;
} KDFParamsData_v0;

#define	SizeOfKDFParamsData_v0	(offsetof(KDFParamsData_v0, crc) + sizeof(pg_crc32c))

/*
 * Parameters of the key derivation function.
 *
 * The parameters are generated by initdb and stored into a file, which is
 * then read during PG startup. This is similar to storing various settings in
 * pg_control. However an existing KDF file is read only, so it does not have
 * to be stored in shared memory.
 */
typedef struct KDFParamsData
{
	/*
	 * The key derivation function number is actually stored in the lowest
	 * byte. The 2nd lowest byte determines the KDF file version, the other
	 * bytes are unused. XXX The KDFKind type is used for historical reasons
	 * (there was no version number in TDE 1.0) - is it worth changing to
	 * another type of the same size?
	 */
#define KDF_FUNCTION_MASK	0x000f
#define KDF_VERSION_MASK	0x00f0
#define	KDF_CURRENT_VERSION		0x01
#define KDF_VERSION_SHIFT		4 /* Shift KDF_CURRENT_VERSION to match
								   * KDF_VERSION_MASK */
	KFDKind		func_version;

	/*
	 * Function-specific parameters.
	 */
	union
	{
		KDFParamsPBKDF2 pbkdf2;
	}			data;

	/*
	 * Encryption key length in bytes.
	 *
	 * This information is also stored in the control file, but pg_keytool
	 * might need it to derive the key for initdb at the time the control file
	 * does not exist yet.
	 */
	uint8		key_len;

	/* CRC of all above ... MUST BE LAST! */
	pg_crc32c	crc;
} KDFParamsData;

#define	SizeOfKDFParamsData	(offsetof(KDFParamsData, crc) + sizeof(pg_crc32c))

/*
 * Pointer to the KDF parameters.
 */
static KDFParamsData KDFParams;

/* Initialize KDF file. */
void
init_kdf(int key_len)
{
	KDFParamsPBKDF2 *params;
	struct timeval tv;
	uint64	salt;

	memset(&KDFParams, 0, SizeOfKDFParamsData);
	KDFParams.func_version = (KDF_CURRENT_VERSION << KDF_VERSION_SHIFT) |
		KDF_OPENSSL_PKCS5_PBKDF2_HMAC_SHA;
	params = &KDFParams.data.pbkdf2;

	/*
	 * Currently we derive the salt in the same way as system identifier,
	 * however these two values are not supposed to match. XXX Is it worth the
	 * effort if initdb derives the system identifier, passes it to this
	 * function and also sends it to the bootstrap process? Not sure.
	 */
	gettimeofday(&tv, NULL);
	salt = ((uint64) tv.tv_sec) << 32;
	salt |= ((uint64) tv.tv_usec) << 12;
	salt |= getpid() & 0xFFF;

	memcpy(params->salt, &salt, sizeof(uint64));
	params->niter = ENCRYPTION_KDF_NITER;

	KDFParams.key_len = key_len;
}

/*
 * Write KDFParamsData to file.
 */
void
write_kdf_file(char *dir)
{
	char		path[MAXPGPATH];
	int			fd;

	/* Account for both file separator and terminating NULL character. */
	if ((strlen(dir) + 1 + strlen(KDF_PARAMS_FILE) + 1) > MAXPGPATH)
	{
		pg_log_fatal("KDF directory is too long");
		exit(EXIT_FAILURE);
	}

	snprintf(path, MAXPGPATH, "%s/%s", dir, KDF_PARAMS_FILE);

	/* Contents are protected with a CRC */
	INIT_CRC32C(KDFParams.crc);
	COMP_CRC32C(KDFParams.crc,
				(char *) &KDFParams,
				offsetof(KDFParamsData, crc));
	FIN_CRC32C(KDFParams.crc);

	fd = open(path, O_WRONLY | O_CREAT | PG_BINARY,
			  pg_file_create_mode);
	if (fd < 0)
	{
		pg_log_fatal("could not create key derivation file \"%s\": %m", path);
		exit(EXIT_FAILURE);
	}

	if (write(fd, &KDFParams, KDF_PARAMS_FILE_SIZE) != KDF_PARAMS_FILE_SIZE)
	{
		/* if write didn't set errno, assume problem is no disk space */
		if (errno == 0)
			errno = ENOSPC;
		pg_log_fatal("could not write to key derivation file \"%s\": %m",
					 path);
		exit(EXIT_FAILURE);
	}

	if (close(fd))
	{
		pg_log_fatal("could not close key setup file: %m");
		exit(EXIT_FAILURE);
	}
}

/*
 * Read KDFParamsData from file and store it in local memory.
 *
 * If dir is NULL, assume we're in the data directory.
 *
 * Returns key length in bytes.
 */
int
read_kdf_file(char *dir)
{
	pg_crc32c	crc;
	char		path[MAXPGPATH];
	int			fd;
	KFDKind		func_version;
	int		version;
	char	*c, *end;
	int		remains;

	/* Account for both file separator and terminating NULL character. */
	if ((strlen(dir) + 1 + strlen(KDF_PARAMS_FILE) + 1) > MAXPGPATH)
	{
		pg_log_fatal("KDF directory is too long");
		exit(EXIT_FAILURE);
	}

	snprintf(path, MAXPGPATH, "%s/%s", dir, KDF_PARAMS_FILE);

	fd = open(path, O_RDONLY | PG_BINARY, S_IRUSR);
	if (fd < 0)
	{
		pg_log_fatal("could not open key setup file \"%s\": %m", path);
		exit(EXIT_FAILURE);
	}

	/* First, check which version we're reading. */
	if (read(fd, &func_version, sizeof(func_version)) != sizeof(func_version))
	{
		pg_log_fatal("could not read from key setup file \"%s\": %m", path);
		exit(EXIT_FAILURE);
	}
	version = (func_version & KDF_VERSION_MASK) >> KDF_VERSION_SHIFT;
	if (version == 0)
	{
		KDFParamsData_v0	params_v0;

		/* Finish reading of the old version. */
		c = (char *) &params_v0.function + sizeof(params_v0.function);
		end = (char *) &params_v0 + SizeOfKDFParamsData_v0;
		remains = end - c;
		if (read(fd, c, remains) != remains)
		{
			pg_log_fatal("could not read from key setup file \"%s\": %m", path);
			exit(EXIT_FAILURE);
		}
		close(fd);

		/* Assign the value we read earlier so that CRC can be computed. */
		params_v0.function = func_version;

		/* Now check the CRC. */
		INIT_CRC32C(crc);
		COMP_CRC32C(crc,
					(char *) &params_v0,
					offsetof(KDFParamsData_v0, crc));
		FIN_CRC32C(crc);

		if (!EQ_CRC32C(crc, params_v0.crc))
		{
			pg_log_fatal("incorrect checksum in key setup file \"%s\"", path);
			exit(EXIT_FAILURE);
		}

		/* If the version is 0, func_version should match the function. */
		if (func_version != KDF_OPENSSL_PKCS5_PBKDF2_HMAC_SHA)
		{
			pg_log_fatal("unsupported KDF function");
			exit(EXIT_FAILURE);
		}

		/* Initialize the current version. */
		KDFParams.func_version = func_version |
			(KDF_CURRENT_VERSION << KDF_VERSION_SHIFT);
		KDFParams.data.pbkdf2 = params_v0.data.pbkdf2;
		/* KDF v0 belongs to TDE 1.0, so no choice of the key length. */
		KDFParams.key_len = DEFAULT_ENCRYPTION_KEY_LENGTH;
	}
	else
	{
		Assert(version == KDF_CURRENT_VERSION);

		/* Finish reading of the current version. */
		c = (char *) &KDFParams.func_version + sizeof(KDFParams.func_version);
		end = (char *) &KDFParams + SizeOfKDFParamsData;
		remains = end - c;
		if (read(fd, c, remains) != remains)
		{
			pg_log_fatal("could not read from key setup file \"%s\": %m", path);
			exit(EXIT_FAILURE);
		}
		close(fd);

		/* Assign the value we read earlier so that CRC can be computed. */
		KDFParams.func_version = func_version;

		/* Now check the CRC. */
		INIT_CRC32C(crc);
		COMP_CRC32C(crc,
					(char *) &KDFParams,
					offsetof(KDFParamsData, crc));
		FIN_CRC32C(crc);

		if (!EQ_CRC32C(crc, KDFParams.crc))
		{
			pg_log_fatal("incorrect checksum in key setup file \"%s\"", path);
			exit(EXIT_FAILURE);
		}

		/* Check the function number. */
		if ((func_version & KDF_FUNCTION_MASK) !=
			KDF_OPENSSL_PKCS5_PBKDF2_HMAC_SHA)
		{
			pg_log_fatal("unsupported KDF function");
			exit(EXIT_FAILURE);
		}
	}

	return KDFParams.key_len;
}

/*
 * Run the key derivation function and initialize encryption_key variable.
 */
void
derive_key_from_password(unsigned char *encryption_key, const char *password,
						 int len)
{
	KDFParamsPBKDF2 *params;
	int			rc;

	params = &KDFParams.data.pbkdf2;
	rc = PKCS5_PBKDF2_HMAC(password,
						   len,
						   params->salt,
						   ENCRYPTION_KDF_SALT_LEN,
						   params->niter,
						   EVP_sha1(),
						   KDFParams.key_len,
						   encryption_key);

	if (rc != 1)
	{
		pg_log_fatal("failed to derive key from password");
		exit(EXIT_FAILURE);
	}
}

/*
 * Send the contents of encryption_key in the form of special startup packet
 * to a server that is being started.
 *
 * Returns true if we could send the message and false if not, however even
 * success does not guarantee that server started up - caller should
 * eventually test server connection himself. On failure, save pointer to
 * error message into args->error_msg.
 */
bool
send_key_to_postmaster(SendKeyArgs *args)
{
	const char **keywords = pg_malloc0(3 * sizeof(*keywords));
	const char **values = pg_malloc0(3 * sizeof(*values));
	int	i;
	PGconn *conn = NULL;
	EncryptionKeyMsg	message;
	int	msg_size;

/* How many seconds we can wait for the postmaster to receive the key. */
#define SEND_ENCRYPT_KEY_TIMEOUT	60

	if (args->host)
	{
		keywords[0] = "host";
		values[0] = args->host;
	}
	if (args->port)
	{
		keywords[1] = "port";
		values[1] = args->port;
	}

	/* Compose the message. */
	message.encryptionKeyCode = pg_hton32(ENCRYPTION_KEY_MSG_CODE);
	message.version = 1;
	if (args->encryption_key)
	{
		memcpy(message.data, args->encryption_key, ENCRYPTION_KEY_MAX_LENGTH);
		message.empty = false;
	}
	else
		message.empty = true;
	msg_size = offsetof(EncryptionKeyMsg, data) + ENCRYPTION_KEY_MAX_LENGTH;

	for (i = 0; i < SEND_ENCRYPT_KEY_TIMEOUT + 1; i++)
	{
		char	sslmode;

		if (i > 0)
			/* Sleep for 1 second. */
			pg_usleep(1000000L);

		/* Has the postmaster crashed? */
		if (args->pm_pid != 0)
		{
#ifndef WIN32
			int			exitstatus;

			if (waitpid((pid_t) args->pm_pid, &exitstatus, WNOHANG) ==
				(pid_t) args->pm_pid)
			{
				args->pm_exited = true;
				return false;
			}
#else
			if (WaitForSingleObject(args->pmProcess, 0) == WAIT_OBJECT_0)
			{
				args->pm_exited = true;
				return false;
			}
#endif
		}

		if (conn)
		{
			PQfinish(conn);
			conn = NULL;
		}

		/*
		 * Although we don't expect the server to accept regular libpq
		 * messages at the moment, try to get at least a valid socket.
		 */
		conn = PQconnectStartParams(keywords, values, false);
		if (conn == NULL)
			continue;

		if (PQstatus(conn) != CONNECTION_STARTED &&
			PQstatus(conn) != CONNECTION_MADE)
		{
			char	*msg = PQerrorMessage(conn);

			if (msg && strlen(msg) > 0)
				args->error_msg = pstrdup(msg);

			continue;
		}

		/*
		 * Unless we're sending the key via unix socket, take SSL mode into
		 * account ('d' ~ "disable", 'a' ~ "allow", 'p' ~ "prefer"). "allow"
		 * and "prefer" modes are not useful in terms of security, and
		 * especially "allow" would be much trickier for
		 * PQconnectSSLHandshake() to handle.
		 */
		sslmode = PQconnectionSSLMode(conn);
		if (sslmode != 'd' && sslmode != 'a' && sslmode != 'p' &&
			!PQconnectedToSocket(conn))
		{
			if (!PQconnectSSLHandshake(conn))
			{
				char	*msg = PQerrorMessage(conn);

				if (msg && strlen(msg) > 0)
					args->error_msg = pstrdup(msg);

				/*
				 * If the socket could be opened but SSL is not available, the
				 * next try probably won't help.
				 */
				PQfinish(conn);
				return false;
			}
		}

		/* Send the packet. */
		if (!PQpacketSend(conn, (char *) &message, msg_size))
		{
			args->error_msg = pstrdup(PQerrorMessage(conn));
			return false;
		}

		/* Success */
		break;
	}

	pg_free(keywords);
	pg_free(values);
	if (conn)
		PQfinish(conn);

	return true;
}
#endif	/* USE_ENCRYPTION */
