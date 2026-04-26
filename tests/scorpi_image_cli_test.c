/* scorpi-image CLI coverage. */

#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static const char *
cli_path(void)
{
	const char *path;

	path = getenv("SCORPI_IMAGE_CLI");
	assert(path != NULL && path[0] != '\0');
	return (path);
}

static void
run_ok(const char *cmd)
{
	int status;

	status = system(cmd);
	assert(status != -1);
	assert(WIFEXITED(status));
	assert(WEXITSTATUS(status) == 0);
}

static void
run_fail(const char *cmd)
{
	int status;

	status = system(cmd);
	assert(status != -1);
	assert(WIFEXITED(status));
	assert(WEXITSTATUS(status) != 0);
}

static void
test_create_info_check_mb_suffix(void)
{
	char path[] = "/tmp/scorpi-image-cli-create-XXXXXX";
	char cmd[1024];
	char output_path[128];
	FILE *fp;
	char output[512];
	int fd;

	fd = mkstemp(path);
	assert(fd >= 0);
	assert(close(fd) == 0);
	assert(unlink(path) == 0);

	snprintf(cmd, sizeof(cmd),
	    "%s create --format sco --size 128mb %s",
	    cli_path(), path);
	run_ok(cmd);

	snprintf(cmd, sizeof(cmd), "%s check %s", cli_path(), path);
	run_ok(cmd);

	snprintf(cmd, sizeof(cmd), "%s info %s > %s.info", cli_path(), path,
	    path);
	run_ok(cmd);
	snprintf(output_path, sizeof(output_path), "%s.info", path);
	fp = fopen(output_path, "r");
	assert(fp != NULL);
	memset(output, 0, sizeof(output));
	assert(fread(output, 1, sizeof(output) - 1, fp) > 0);
	assert(fclose(fp) == 0);
	assert(strstr(output, "format=sco\n") != NULL);
	assert(strstr(output, "virtual_size=134217728\n") != NULL);
	assert(strstr(output, "cluster_size=262144\n") != NULL);
	assert(strstr(output, "base_uri=") == NULL);
	assert(unlink(output_path) == 0);
	assert(unlink(path) == 0);
}

static void
test_create_info_check_raw_bytes(void)
{
	char path[] = "/tmp/scorpi-image-cli-bytes-XXXXXX";
	char cmd[1024];
	char output_path[128];
	FILE *fp;
	char output[512];
	int fd;

	fd = mkstemp(path);
	assert(fd >= 0);
	assert(close(fd) == 0);
	assert(unlink(path) == 0);

	snprintf(cmd, sizeof(cmd),
	    "%s create --format sco --size 134217728 %s",
	    cli_path(), path);
	run_ok(cmd);
	snprintf(cmd, sizeof(cmd), "%s info %s > %s.info", cli_path(), path,
	    path);
	run_ok(cmd);
	snprintf(output_path, sizeof(output_path), "%s.info", path);
	fp = fopen(output_path, "r");
	assert(fp != NULL);
	memset(output, 0, sizeof(output));
	assert(fread(output, 1, sizeof(output) - 1, fp) > 0);
	assert(fclose(fp) == 0);
	assert(strstr(output, "virtual_size=134217728\n") != NULL);
	assert(unlink(output_path) == 0);
	assert(unlink(path) == 0);
}

static void
test_create_with_positional_base(void)
{
	char path[] = "/tmp/scorpi-image-cli-base-XXXXXX";
	char cmd[1024];
	char output_path[128];
	FILE *fp;
	char output[512];
	int fd;

	fd = mkstemp(path);
	assert(fd >= 0);
	assert(close(fd) == 0);
	assert(unlink(path) == 0);

	snprintf(cmd, sizeof(cmd),
	    "%s create --format sco --size 128mb %s base.raw",
	    cli_path(), path);
	run_ok(cmd);
	snprintf(cmd, sizeof(cmd), "%s info %s > %s.info", cli_path(), path,
	    path);
	run_ok(cmd);
	snprintf(output_path, sizeof(output_path), "%s.info", path);
	fp = fopen(output_path, "r");
	assert(fp != NULL);
	memset(output, 0, sizeof(output));
	assert(fread(output, 1, sizeof(output) - 1, fp) > 0);
	assert(fclose(fp) == 0);
	assert(strstr(output, "base_uri=file:base.raw\n") != NULL);
	assert(unlink(output_path) == 0);
	assert(unlink(path) == 0);
}

static void
test_check_rejects_corrupt_image_gb_suffix(void)
{
	char path[] = "/tmp/scorpi-image-cli-corrupt-XXXXXX";
	char cmd[1024];
	uint8_t bad;
	int fd;

	fd = mkstemp(path);
	assert(fd >= 0);
	assert(close(fd) == 0);
	assert(unlink(path) == 0);

	snprintf(cmd, sizeof(cmd),
	    "%s create --format sco --size 1gb %s", cli_path(), path);
	run_ok(cmd);
	fd = open(path, O_RDWR);
	assert(fd >= 0);
	bad = 0xff;
	assert(pwrite(fd, &bad, 1, 0) == 1);
	assert(close(fd) == 0);

	snprintf(cmd, sizeof(cmd), "%s check %s", cli_path(), path);
	run_fail(cmd);
	assert(unlink(path) == 0);
}

static void
test_removed_create_options_rejected(void)
{
	char path[] = "/tmp/scorpi-image-cli-options-XXXXXX";
	char cmd[1024];
	int fd;

	fd = mkstemp(path);
	assert(fd >= 0);
	assert(close(fd) == 0);
	assert(unlink(path) == 0);

	snprintf(cmd, sizeof(cmd),
	    "%s create --format sco --size 128mb --cluster-size 65536 %s",
	    cli_path(), path);
	run_fail(cmd);
	snprintf(cmd, sizeof(cmd),
	    "%s create --format sco --size 128mb --base file:base.raw %s",
	    cli_path(), path);
	run_fail(cmd);
	snprintf(cmd, sizeof(cmd),
	    "%s create --format sco --size 128mb --sealed %s",
	    cli_path(), path);
	run_fail(cmd);
}

static void
read_info_output(const char *path, char *output, size_t output_len)
{
	char cmd[1024];
	char output_path[128];
	FILE *fp;

	snprintf(cmd, sizeof(cmd), "%s info %s > %s.info", cli_path(), path,
	    path);
	run_ok(cmd);
	snprintf(output_path, sizeof(output_path), "%s.info", path);
	fp = fopen(output_path, "r");
	assert(fp != NULL);
	memset(output, 0, output_len);
	assert(fread(output, 1, output_len - 1, fp) > 0);
	assert(fclose(fp) == 0);
	assert(unlink(output_path) == 0);
}

static void
test_seal_and_unseal(void)
{
	char path[] = "/tmp/scorpi-image-cli-seal-XXXXXX";
	char cmd[1024];
	char output[512];
	int fd;

	fd = mkstemp(path);
	assert(fd >= 0);
	assert(close(fd) == 0);
	assert(unlink(path) == 0);

	snprintf(cmd, sizeof(cmd),
	    "%s create --format sco --size 128mb %s",
	    cli_path(), path);
	run_ok(cmd);
	read_info_output(path, output, sizeof(output));
	assert(strstr(output, "sealed=false\n") != NULL);

	snprintf(cmd, sizeof(cmd), "%s seal %s", cli_path(), path);
	run_ok(cmd);
	snprintf(cmd, sizeof(cmd), "%s seal %s", cli_path(), path);
	run_ok(cmd);
	read_info_output(path, output, sizeof(output));
	assert(strstr(output, "sealed=true\n") != NULL);

	snprintf(cmd, sizeof(cmd), "%s unseal %s", cli_path(), path);
	run_ok(cmd);
	snprintf(cmd, sizeof(cmd), "%s unseal %s", cli_path(), path);
	run_ok(cmd);
	read_info_output(path, output, sizeof(output));
	assert(strstr(output, "sealed=false\n") != NULL);

	assert(unlink(path) == 0);
}

static void
test_seal_rejects_non_sco(void)
{
	char path[] = "/tmp/scorpi-image-cli-seal-raw-XXXXXX";
	char cmd[1024];
	uint8_t buf[512];
	int fd;

	fd = mkstemp(path);
	assert(fd >= 0);
	memset(buf, 0x5a, sizeof(buf));
	assert(write(fd, buf, sizeof(buf)) == (ssize_t)sizeof(buf));
	assert(close(fd) == 0);

	snprintf(cmd, sizeof(cmd), "%s seal %s", cli_path(), path);
	run_fail(cmd);
	snprintf(cmd, sizeof(cmd), "%s unseal %s", cli_path(), path);
	run_fail(cmd);
	assert(unlink(path) == 0);
}

int
main(void)
{
	test_create_info_check_mb_suffix();
	test_create_with_positional_base();
	test_create_info_check_raw_bytes();
	test_check_rejects_corrupt_image_gb_suffix();
	test_removed_create_options_rejected();
	test_seal_and_unseal();
	test_seal_rejects_non_sco();
	return (0);
}
