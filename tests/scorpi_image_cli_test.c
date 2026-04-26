/* scorpi_image CLI coverage. */

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
test_create_info_check(void)
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
	    "%s create --format sco --size 134217728 --base file:base.raw %s",
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
	assert(strstr(output, "base_uri=file:base.raw\n") != NULL);
	assert(unlink(output_path) == 0);
	assert(unlink(path) == 0);
}

static void
test_check_rejects_corrupt_image(void)
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
	    "%s create --format sco --size 134217728 %s", cli_path(), path);
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

int
main(void)
{
	test_create_info_check();
	test_check_rejects_corrupt_image();
	return (0);
}
