// SPDX-License-Identifier: LGPL-2.1-or-later
/*
 * This file is part of libgpiod.
 *
 * Copyright (C) 2017-2018 Bartosz Golaszewski <bartekgola@gmail.com>
 */

/* Low-level, core library code. */

#include <errno.h>
#include <fcntl.h>
#include <gpiod.h>
#include <linux/gpio.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/types.h>
#include <unistd.h>

enum {
	LINE_FREE = 0,
	LINE_REQUESTED_VALUES,
	LINE_REQUESTED_EVENTS,
};

struct line_fd_handle {
	int fd;
	int refcount;
};

struct gpiod_line {
	unsigned int offset;
	int direction;
	int active_state;
	bool used;
	bool open_source;
	bool open_drain;

	int state;
	bool up_to_date;

	struct gpiod_chip *chip;
	struct line_fd_handle *fd_handle;

	char name[32];
	char consumer[32];
};

struct gpiod_chip {
	struct gpiod_line **lines;
	unsigned int num_lines;

	int fd;

	char name[32];
	char label[32];
};

static bool is_gpiochip_cdev(const char *path)
{
	char *name, *pathcpy, *sysfsp, sysfsdev[16], devstr[16];
	struct stat statbuf;
	bool ret = false;
	int rv, fd;
	ssize_t rd;

	rv = lstat(path, &statbuf);
	if (rv)
		goto out;

	/* Is it a character device? */
	if (!S_ISCHR(statbuf.st_mode)) {
		/*
		 * Passing a file descriptor not associated with a character
		 * device to ioctl() makes it set errno to ENOTTY. Let's do
		 * the same in order to stay compatible with the versions of
		 * libgpiod from before the introduction of this routine.
		 */
		errno = ENOTTY;
		goto out;
	}

	/* Get the basename. */
	pathcpy = strdup(path);
	if (!pathcpy)
		goto out;

	name = basename(pathcpy);

	/* we are not going to simulate sysfs attributes */

	/* Do we have a corresponding sysfs attribute? */
	/* rv = asprintf(&sysfsp, "/sys/bus/gpio/devices/%s/dev", name);
	if (rv < 0)
		goto out_free_pathcpy; */

	/* if (access(sysfsp, R_OK) != 0) { */
		/*
		 * This is a character device but not the one we're after.
		 * Before the introduction of this function, we'd fail with
		 * ENOTTY on the first GPIO ioctl() call for this file
		 * descriptor. Let's stay compatible here and keep returning
		 * the same error code.
		 */
	/*	errno = ENOTTY;
		goto out_free_sysfsp;
	} */

	/*
	 * Make sure the major and minor numbers of the character device
	 * correspond with the ones in the dev attribute in sysfs.
	 */
	/* snprintf(devstr, sizeof(devstr), "%u:%u",
		 major(statbuf.st_rdev), minor(statbuf.st_rdev));

	fd = open(sysfsp, O_RDONLY);
	if (fd < 0)
		goto out_free_sysfsp;

	memset(sysfsdev, 0, sizeof(sysfsdev));
	rd = read(fd, sysfsdev, strlen(devstr));
	close(fd);
	if (rd < 0)
		goto out_free_sysfsp;

	if (strcmp(sysfsdev, devstr) != 0) {
		errno = ENODEV;
		goto out_free_sysfsp;
	} */

	ret = true;

out_free_sysfsp:
	free(sysfsp);
out_free_pathcpy:
	free(pathcpy);
out:
	return ret;
}

struct gpiod_chip *gpiod_chip_open(const char *path)
{
	struct gpiochip_info info;
	struct gpiod_chip *chip;
	int rv, fd;

	fd = open(path, O_RDWR | O_CLOEXEC);
	if (fd < 0)
		return NULL;

	/*
	 * We were able to open the file but is it really a gpiochip character
	 * device?
	 */
	if (!is_gpiochip_cdev(path)) {
		close(fd);
		return NULL;
	}

	chip = malloc(sizeof(*chip));
	if (!chip)
		goto err_close_fd;

	memset(chip, 0, sizeof(*chip));
	memset(&info, 0, sizeof(info));

	rv = ioctl(fd, GPIO_GET_CHIPINFO_IOCTL, &info);
	if (rv < 0)
		goto err_free_chip;

	chip->fd = fd;
	chip->num_lines = info.lines;

	/*
	 * GPIO device must have a name - don't bother checking this field. In
	 * the worst case (would have to be a weird kernel bug) it'll be empty.
	 */
	strncpy(chip->name, info.name, sizeof(chip->name));

	/*
	 * The kernel sets the label of a GPIO device to "unknown" if it
	 * hasn't been defined in DT, board file etc. On the off-chance that
	 * we got an empty string, do the same.
	 */
	if (info.label[0] == '\0')
		strncpy(chip->label, "unknown", sizeof(chip->label));
	else
		strncpy(chip->label, info.label, sizeof(chip->label));

	return chip;

err_free_chip:
	free(chip);
err_close_fd:
	close(fd);

	return NULL;
}

void gpiod_chip_close(struct gpiod_chip *chip)
{
	struct gpiod_line *line;
	unsigned int i;

	if (chip->lines) {
		for (i = 0; i < chip->num_lines; i++) {
			line = chip->lines[i];
			if (line) {
				gpiod_line_release(line);
				free(line);
			}
		}

		free(chip->lines);
	}

	close(chip->fd);
	free(chip);
}

const char *gpiod_chip_name(struct gpiod_chip *chip)
{
	return chip->name;
}

const char *gpiod_chip_label(struct gpiod_chip *chip)
{
	return chip->label;
}

unsigned int gpiod_chip_num_lines(struct gpiod_chip *chip)
{
	return chip->num_lines;
}

struct gpiod_line *
gpiod_chip_get_line(struct gpiod_chip *chip, unsigned int offset)
{
	struct gpiod_line *line;
	int rv;

	if (offset >= chip->num_lines) {
		errno = EINVAL;
		return NULL;
	}

	if (!chip->lines) {
		chip->lines = calloc(chip->num_lines,
				     sizeof(struct gpiod_line *));
		if (!chip->lines)
			return NULL;
	}

	if (!chip->lines[offset]) {
		line = malloc(sizeof(*line));
		if (!line)
			return NULL;

		memset(line, 0, sizeof(*line));

		line->offset = offset;
		line->chip = chip;

		chip->lines[offset] = line;
	} else {
		line = chip->lines[offset];
	}

	rv = gpiod_line_update(line);
	if (rv < 0)
		return NULL;

	return line;
}

static struct line_fd_handle *line_make_fd_handle(int fd)
{
	struct line_fd_handle *handle;

	handle = malloc(sizeof(*handle));
	if (!handle)
		return NULL;

	handle->fd = fd;
	handle->refcount = 0;

	return handle;
}

static void line_fd_incref(struct gpiod_line *line)
{
	line->fd_handle->refcount++;
}

static void line_fd_decref(struct gpiod_line *line)
{
	struct line_fd_handle *handle = line->fd_handle;

	handle->refcount--;

	if (handle->refcount == 0) {
		close(handle->fd);
		free(handle);
		line->fd_handle = NULL;
	}
}

static void line_set_fd(struct gpiod_line *line, struct line_fd_handle *handle)
{
	line->fd_handle = handle;
	line_fd_incref(line);
}

static int line_get_fd(struct gpiod_line *line)
{
	return line->fd_handle->fd;
}

static void line_maybe_update(struct gpiod_line *line)
{
	int rv;

	rv = gpiod_line_update(line);
	if (rv < 0)
		line->up_to_date = false;
}

struct gpiod_chip *gpiod_line_get_chip(struct gpiod_line *line)
{
	return line->chip;
}

unsigned int gpiod_line_offset(struct gpiod_line *line)
{
	return line->offset;
}

const char *gpiod_line_name(struct gpiod_line *line)
{
	return line->name[0] == '\0' ? NULL : line->name;
}

const char *gpiod_line_consumer(struct gpiod_line *line)
{
	return line->consumer[0] == '\0' ? NULL : line->consumer;
}

int gpiod_line_direction(struct gpiod_line *line)
{
	return line->direction;
}

int gpiod_line_active_state(struct gpiod_line *line)
{
	return line->active_state;
}

bool gpiod_line_is_used(struct gpiod_line *line)
{
	return line->used;
}

bool gpiod_line_is_open_drain(struct gpiod_line *line)
{
	return line->open_drain;
}

bool gpiod_line_is_open_source(struct gpiod_line *line)
{
	return line->open_source;
}

bool gpiod_line_needs_update(struct gpiod_line *line)
{
	return !line->up_to_date;
}

int gpiod_line_update(struct gpiod_line *line)
{
	struct gpioline_info info;
	int rv;

	memset(&info, 0, sizeof(info));
	info.line_offset = line->offset;

	rv = ioctl(line->chip->fd, GPIO_GET_LINEINFO_IOCTL, &info);
	if (rv < 0)
		return -1;

	line->direction = info.flags & GPIOLINE_FLAG_IS_OUT
						? GPIOD_LINE_DIRECTION_OUTPUT
						: GPIOD_LINE_DIRECTION_INPUT;
	line->active_state = info.flags & GPIOLINE_FLAG_ACTIVE_LOW
						? GPIOD_LINE_ACTIVE_STATE_LOW
						: GPIOD_LINE_ACTIVE_STATE_HIGH;

	line->used = info.flags & GPIOLINE_FLAG_KERNEL;
	line->open_drain = info.flags & GPIOLINE_FLAG_OPEN_DRAIN;
	line->open_source = info.flags & GPIOLINE_FLAG_OPEN_SOURCE;

	strncpy(line->name, info.name, sizeof(line->name));
	strncpy(line->consumer, info.consumer, sizeof(line->consumer));

	line->up_to_date = true;

	return 0;
}

static bool line_bulk_same_chip(struct gpiod_line_bulk *bulk)
{
	struct gpiod_line *first_line, *line;
	struct gpiod_chip *first_chip, *chip;
	unsigned int i;

	if (bulk->num_lines == 1)
		return true;

	first_line = gpiod_line_bulk_get_line(bulk, 0);
	first_chip = gpiod_line_get_chip(first_line);

	for (i = 1; i < bulk->num_lines; i++) {
		line = bulk->lines[i];
		chip = gpiod_line_get_chip(line);

		if (first_chip != chip) {
			errno = EINVAL;
			return false;
		}
	}

	return true;
}

static bool line_bulk_all_requested(struct gpiod_line_bulk *bulk)
{
	struct gpiod_line *line, **lineptr;

	gpiod_line_bulk_foreach_line(bulk, line, lineptr) {
		if (!gpiod_line_is_requested(line)) {
			errno = EPERM;
			return false;
		}
	}

	return true;
}

static bool line_bulk_all_free(struct gpiod_line_bulk *bulk)
{
	struct gpiod_line *line, **lineptr;

	gpiod_line_bulk_foreach_line(bulk, line, lineptr) {
		if (!gpiod_line_is_free(line)) {
			errno = EBUSY;
			return false;
		}
	}

	return true;
}

static int line_request_values(struct gpiod_line_bulk *bulk,
			       const struct gpiod_line_request_config *config,
			       const int *default_vals)
{
	struct gpiod_line *line, **lineptr;
	struct line_fd_handle *line_fd;
	struct gpiohandle_request req;
	unsigned int i;
	int rv, fd;

	if ((config->request_type != GPIOD_LINE_REQUEST_DIRECTION_OUTPUT) &&
	    (config->flags & (GPIOD_LINE_REQUEST_FLAG_OPEN_DRAIN |
			      GPIOD_LINE_REQUEST_FLAG_OPEN_SOURCE))) {
		errno = EINVAL;
		return -1;
	}

	if ((config->flags & GPIOD_LINE_REQUEST_FLAG_OPEN_DRAIN) &&
	    (config->flags & GPIOD_LINE_REQUEST_FLAG_OPEN_SOURCE)) {
		errno = EINVAL;
		return -1;
	}

	memset(&req, 0, sizeof(req));

	if (config->flags & GPIOD_LINE_REQUEST_FLAG_OPEN_DRAIN)
		req.flags |= GPIOHANDLE_REQUEST_OPEN_DRAIN;
	if (config->flags & GPIOD_LINE_REQUEST_FLAG_OPEN_SOURCE)
		req.flags |= GPIOHANDLE_REQUEST_OPEN_SOURCE;
	if (config->flags & GPIOD_LINE_REQUEST_FLAG_ACTIVE_LOW)
		req.flags |= GPIOHANDLE_REQUEST_ACTIVE_LOW;

	if (config->request_type == GPIOD_LINE_REQUEST_DIRECTION_INPUT)
		req.flags |= GPIOHANDLE_REQUEST_INPUT;
	else if (config->request_type == GPIOD_LINE_REQUEST_DIRECTION_OUTPUT)
		req.flags |= GPIOHANDLE_REQUEST_OUTPUT;

	req.lines = gpiod_line_bulk_num_lines(bulk);

	gpiod_line_bulk_foreach_line_off(bulk, line, i) {
		req.lineoffsets[i] = gpiod_line_offset(line);
		if (config->request_type ==
				GPIOD_LINE_REQUEST_DIRECTION_OUTPUT &&
		    default_vals)
			req.default_values[i] = !!default_vals[i];
	}

	if (config->consumer)
		strncpy(req.consumer_label, config->consumer,
			sizeof(req.consumer_label) - 1);

	line = gpiod_line_bulk_get_line(bulk, 0);
	fd = line->chip->fd;

	rv = ioctl(fd, GPIO_GET_LINEHANDLE_IOCTL, &req);
	if (rv < 0)
		return -1;

	line_fd = line_make_fd_handle(req.fd);
	if (!line_fd)
		return -1;

	gpiod_line_bulk_foreach_line(bulk, line, lineptr) {
		line->state = LINE_REQUESTED_VALUES;
		line_set_fd(line, line_fd);
		line_maybe_update(line);
	}

	return 0;
}

static int line_request_event_single(struct gpiod_line *line,
			const struct gpiod_line_request_config *config)
{
	struct line_fd_handle *line_fd;
	struct gpioevent_request req;
	int rv;

	memset(&req, 0, sizeof(req));

	if (config->consumer)
		strncpy(req.consumer_label, config->consumer,
			sizeof(req.consumer_label) - 1);

	req.lineoffset = gpiod_line_offset(line);
	req.handleflags |= GPIOHANDLE_REQUEST_INPUT;

	if (config->flags & GPIOD_LINE_REQUEST_FLAG_OPEN_DRAIN)
		req.handleflags |= GPIOHANDLE_REQUEST_OPEN_DRAIN;
	if (config->flags & GPIOD_LINE_REQUEST_FLAG_OPEN_SOURCE)
		req.handleflags |= GPIOHANDLE_REQUEST_OPEN_SOURCE;
	if (config->flags & GPIOD_LINE_REQUEST_FLAG_ACTIVE_LOW)
		req.handleflags |= GPIOHANDLE_REQUEST_ACTIVE_LOW;

	if (config->request_type == GPIOD_LINE_REQUEST_EVENT_RISING_EDGE)
		req.eventflags |= GPIOEVENT_REQUEST_RISING_EDGE;
	else if (config->request_type == GPIOD_LINE_REQUEST_EVENT_FALLING_EDGE)
		req.eventflags |= GPIOEVENT_REQUEST_FALLING_EDGE;
	else if (config->request_type == GPIOD_LINE_REQUEST_EVENT_BOTH_EDGES)
		req.eventflags |= GPIOEVENT_REQUEST_BOTH_EDGES;

	rv = ioctl(line->chip->fd, GPIO_GET_LINEEVENT_IOCTL, &req);
	if (rv < 0)
		return -1;

	line_fd = line_make_fd_handle(req.fd);
	if (!line_fd)
		return -1;

	line->state = LINE_REQUESTED_EVENTS;
	line_set_fd(line, line_fd);
	line_maybe_update(line);

	return 0;
}

static int line_request_events(struct gpiod_line_bulk *bulk,
			       const struct gpiod_line_request_config *config)
{
	struct gpiod_line *line;
	unsigned int off;
	int rv, rev;

	gpiod_line_bulk_foreach_line_off(bulk, line, off) {
		rv = line_request_event_single(line, config);
		if (rv) {
			for (rev = off - 1; rev >= 0; rev--) {
				line = gpiod_line_bulk_get_line(bulk, rev);
				gpiod_line_release(line);
			}

			return -1;
		}
	}

	return 0;
}

int gpiod_line_request(struct gpiod_line *line,
		       const struct gpiod_line_request_config *config,
		       int default_val)
{
	struct gpiod_line_bulk bulk;

	gpiod_line_bulk_init(&bulk);
	gpiod_line_bulk_add(&bulk, line);

	return gpiod_line_request_bulk(&bulk, config, &default_val);
}

static bool line_request_is_direction(int request)
{
	return request == GPIOD_LINE_REQUEST_DIRECTION_AS_IS ||
	       request == GPIOD_LINE_REQUEST_DIRECTION_INPUT ||
	       request == GPIOD_LINE_REQUEST_DIRECTION_OUTPUT;
}

static bool line_request_is_events(int request)
{
	return request == GPIOD_LINE_REQUEST_EVENT_FALLING_EDGE ||
	       request == GPIOD_LINE_REQUEST_EVENT_RISING_EDGE ||
	       request == GPIOD_LINE_REQUEST_EVENT_BOTH_EDGES;
}

int gpiod_line_request_bulk(struct gpiod_line_bulk *bulk,
			    const struct gpiod_line_request_config *config,
			    const int *default_vals)
{
	if (!line_bulk_same_chip(bulk) || !line_bulk_all_free(bulk))
		return -1;

	if (line_request_is_direction(config->request_type))
		return line_request_values(bulk, config, default_vals);
	else if (line_request_is_events(config->request_type))
		return line_request_events(bulk, config);

	errno = EINVAL;
	return -1;
}

void gpiod_line_release(struct gpiod_line *line)
{
	struct gpiod_line_bulk bulk;

	gpiod_line_bulk_init(&bulk);
	gpiod_line_bulk_add(&bulk, line);

	gpiod_line_release_bulk(&bulk);
}

void gpiod_line_release_bulk(struct gpiod_line_bulk *bulk)
{
	struct gpiod_line *line, **lineptr;

	gpiod_line_bulk_foreach_line(bulk, line, lineptr) {
		if (line->state != LINE_FREE) {
			line_fd_decref(line);
			line->state = LINE_FREE;
		}
	}
}

bool gpiod_line_is_requested(struct gpiod_line *line)
{
	return (line->state == LINE_REQUESTED_VALUES ||
		line->state == LINE_REQUESTED_EVENTS);
}

bool gpiod_line_is_free(struct gpiod_line *line)
{
	return line->state == LINE_FREE;
}

int gpiod_line_get_value(struct gpiod_line *line)
{
	struct gpiod_line_bulk bulk;
	int rv, value;

	gpiod_line_bulk_init(&bulk);
	gpiod_line_bulk_add(&bulk, line);

	rv = gpiod_line_get_value_bulk(&bulk, &value);
	if (rv < 0)
		return -1;

	return value;
}

int gpiod_line_get_value_bulk(struct gpiod_line_bulk *bulk, int *values)
{
	struct gpiohandle_data data;
	struct gpiod_line *first;
	unsigned int i;
	int rv, fd;

	if (!line_bulk_same_chip(bulk) || !line_bulk_all_requested(bulk))
		return -1;

	first = gpiod_line_bulk_get_line(bulk, 0);

	memset(&data, 0, sizeof(data));

	fd = line_get_fd(first);

	rv = ioctl(fd, GPIOHANDLE_GET_LINE_VALUES_IOCTL, &data);
	if (rv < 0)
		return -1;

	for (i = 0; i < gpiod_line_bulk_num_lines(bulk); i++)
		values[i] = data.values[i];

	return 0;
}

int gpiod_line_set_value(struct gpiod_line *line, int value)
{
	struct gpiod_line_bulk bulk;

	gpiod_line_bulk_init(&bulk);
	gpiod_line_bulk_add(&bulk, line);

	return gpiod_line_set_value_bulk(&bulk, &value);
}

int gpiod_line_set_value_bulk(struct gpiod_line_bulk *bulk, const int *values)
{
	struct gpiohandle_data data;
	struct gpiod_line *line;
	unsigned int i;
	int rv, fd;

	if (!line_bulk_same_chip(bulk) || !line_bulk_all_requested(bulk))
		return -1;

	memset(&data, 0, sizeof(data));

	for (i = 0; i < gpiod_line_bulk_num_lines(bulk); i++)
		data.values[i] = (uint8_t)!!values[i];

	line = gpiod_line_bulk_get_line(bulk, 0);
	fd = line_get_fd(line);

	rv = ioctl(fd, GPIOHANDLE_SET_LINE_VALUES_IOCTL, &data);
	if (rv < 0)
		return -1;

	return 0;
}

int gpiod_line_event_wait(struct gpiod_line *line,
			  const struct timespec *timeout)
{
	struct gpiod_line_bulk bulk;

	gpiod_line_bulk_init(&bulk);
	gpiod_line_bulk_add(&bulk, line);

	return gpiod_line_event_wait_bulk(&bulk, timeout, NULL);
}

int gpiod_line_event_wait_bulk(struct gpiod_line_bulk *bulk,
			       const struct timespec *timeout,
			       struct gpiod_line_bulk *event_bulk)
{
	struct pollfd fds[GPIOD_LINE_BULK_MAX_LINES];
	unsigned int off, num_lines;
	struct gpiod_line *line;
	int rv;

	if (!line_bulk_same_chip(bulk) || !line_bulk_all_requested(bulk))
		return -1;

	memset(fds, 0, sizeof(fds));
	num_lines = gpiod_line_bulk_num_lines(bulk);

	gpiod_line_bulk_foreach_line_off(bulk, line, off) {
		fds[off].fd = line_get_fd(line);
		fds[off].events = POLLIN | POLLPRI;
	}

	rv = ppoll(fds, num_lines, timeout, NULL);
	if (rv < 0)
		return -1;
	else if (rv == 0)
		return 0;

	if (event_bulk)
		gpiod_line_bulk_init(event_bulk);

	for (off = 0; off < num_lines; off++) {
		if (fds[off].revents) {
			if (fds[off].revents & POLLNVAL) {
				errno = EINVAL;
				return -1;
			}

			if (event_bulk) {
				line = gpiod_line_bulk_get_line(bulk, off);
				gpiod_line_bulk_add(event_bulk, line);
			}

			if (!--rv)
				break;
		}
	}

	return 1;
}

int gpiod_line_event_read(struct gpiod_line *line,
			  struct gpiod_line_event *event)
{
	int fd;

	if (line->state != LINE_REQUESTED_EVENTS) {
		errno = EPERM;
		return -1;
	}

	fd = line_get_fd(line);

	return gpiod_line_event_read_fd(fd, event);
}

int gpiod_line_event_get_fd(struct gpiod_line *line)
{
	if (line->state != LINE_REQUESTED_EVENTS) {
		errno = EPERM;
		return -1;
	}

	return line_get_fd(line);
}

int gpiod_line_event_read_fd(int fd, struct gpiod_line_event *event)
{
	struct gpioevent_data evdata;
	ssize_t rd;

	memset(&evdata, 0, sizeof(evdata));

	rd = read(fd, &evdata, sizeof(evdata));
	if (rd < 0) {
		return -1;
	} else if (rd != sizeof(evdata)) {
		errno = EIO;
		return -1;
	}

	event->event_type = evdata.id == GPIOEVENT_EVENT_RISING_EDGE
						? GPIOD_LINE_EVENT_RISING_EDGE
						: GPIOD_LINE_EVENT_FALLING_EDGE;

	event->ts.tv_sec = evdata.timestamp / 1000000000ULL;
	event->ts.tv_nsec = evdata.timestamp % 1000000000ULL;

	return 0;
}
