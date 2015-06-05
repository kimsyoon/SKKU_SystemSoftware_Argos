#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <iostream>

#include "common.h"
#include <dlog.h>
#include <glib.h>

#undef LOG_TAG
#define LOG_TAG "ARGOS_SVC"


#include <string.h>
#include <stdarg.h>
#include <errno.h>
#include <dirent.h>
#include <ctype.h>
#include <fcntl.h>
#include <sys/types.h>
#include <pthread.h>


#define NET_TPUT_QOS_NODE	"/dev/network_throughput"
#define NET_BOOST_NODE_1	"/proc/device-tree/argos/boot_device@"
#define NET_BOOST_NODE_2	"/net_boost,node"
#define NET_BOOST_NODE_3	"/net_boost,sysnode"

#define TX_RX_CHECK_NODE	"/proc/net/dev"
#define BASE_NODE			"/proc/device-tree/argos"

#define MAX_STR_BUFF_LEN	128

static char read_buff[MAX_STR_BUFF_LEN];
static GMainLoop* gMainLoop = NULL;

struct monitor_data{
	char node_name[MAX_STR_BUFF_LEN];
	unsigned long long rx;
	unsigned long long tx;
	unsigned long long data;
	unsigned long long delta;
	unsigned long long prev;
	unsigned long long tmp_sum;
	unsigned long prev_tput;
};

char *__read_node(const char *path, char *buf, size_t len)
{
	FILE *fp;
	if (buf == NULL)
		return NULL;
	
	fp = fopen(path, "r");

	if (fp != NULL)
	{
		fgets(buf, len, fp);
		fclose(fp);
	}
	else
		return NULL;
	
	return buf;
}

char *read_node(const char *path)
{
	return __read_node(path, read_buff, MAX_STR_BUFF_LEN);
}

char *trimleft(char *string)
{
	while (*string)
	{
		if (isspace(*string))
			++string;
		else
			break;
	}
	return string;
}

static int nr_devices(const char *name)
{
	unsigned int num = 0;
	DIR *d;
	struct dirent *de;

	d = opendir(name);
	if(d == 0)
	{
		fprintf(stderr, "opendir failed %s\n", strerror(errno));
		closedir(d);
		return -ENOENT;
	}
	
	while ((de = readdir(d)))
	{
		if (!strstr(de->d_name, "@"))
			continue;
		num = num + 1;
	}
	closedir(d);

	return num;
}

static int argos_monitor()
{
	FILE *fp;
	char buffer[1024], name[32], node_str[MAX_STR_BUFF_LEN];
	char *tmp_buf, *tmp_buf1, *tmp_buf2;
	char data_buf[10];
	unsigned Mbps = 0;
	int fd, i, nr_dev;

	nr_dev = nr_devices(BASE_NODE);
	ALOGE("read node from %s = %d\n", BASE_NODE, nr_dev);
	if (nr_dev <= 0)
		return -ENODEV;

	struct monitor_data data[nr_dev];

	fd = open(NET_TPUT_QOS_NODE, O_RDWR);

	if (fd < 0)
	{

		ALOGE("Device doesnt't supprot qos\n");
		return -ENODEV;
	}

	/* Read /proc/device-trr/argos/boot_device@num/net_boost, node */
	for (i = 0; i < nr_dev; i++)
	{
		snprintf(node_str, MAX_STR_BUFF_LEN, "%s%d%s", NET_BOOST_NODE_1, (i + 1), NET_BOOST_NODE_2);
		snprintf(data[i].node_name, MAX_STR_BUFF_LEN, "%s", read_node(node_str));

		data[i].prev = 0;
		data[i].prev_tput = 0;
		ALOGE("%s", data[i].node_name);
	}

	while (1)
	{
		for (i = 0; i < nr_dev; i++)
			data[i].tmp_sum = 0;
			/* if net+boost, node is null, read net_boost, sysnode */
		for (i = 0; i < nr_dev; i++)
		{
			if (!strlen(data[i].node_name))
			{
				snprintf(node_str, MAX_STR_BUFF_LEN, "%s%d%s", NET_BOOST_NODE_1, i + 1, NET_BOOST_NODE_3);

				/* get node value from net_booast, sysnode */
				tmp_buf1 = read_node(node_str);
				tmp_buf2 = read_node(tmp_buf1);
				data[i].data = strtoul(tmp_buf2, NULL, 10);
				data[i].tmp_sum += data[i].data;
			}
			else
			{
				continue;
			}
		}
		
		fp = fopen(TX_RX_CHECK_NODE, "r");
		if (!fp)
		{
			ALOGE("Fail to open node with(%d)\n", errno);
			goto err1;
		}

		/* Ignore unneccssary header data */
		fgets(buffer, sizeof(buffer), fp);
		fgets(buffer, sizeof(buffer), fp);

		while (fgets(buffer, sizeof(buffer), fp))
		{
			buffer[strlen(buffer) - 1] = '\0';

			/* Remove left space */
			tmp_buf = trimleft(buffer);

			for (i = 0; i < nr_dev; i++)
			{
				// ALOGE("TMPBUF: %s\n", tmp_buf);
				sscanf(tmp_buf, "%30[^:]%*[:] %20llu %*s %*s %*s %*s %*s %*s %*s %20llu", name, &data[i].rx, &data[i].tx);
 
				if (strstr(data[i].node_name, name))
				{
					ALOGE("[%s:%d] %s %llu %llu\n", __func__, __LINE__, name, data[i].rx, data[i].tx);
					data[i].tmp_sum += data[i].tx;
					data[i].tmp_sum += data[i].rx;
				}
				else
				{
					continue;
				}
			}
		}

		for (i = 0; i < nr_dev; i++)
		{
			ALOGE("file write (%llu, %llu)\n", data[i].tmp_sum , data[i].prev);
			if (data[i].tmp_sum >= data[i].prev)
			{
				data[i].delta = data[i].tmp_sum - data[i].prev;
				Mbps = (data[i].delta * 8) / 1000;
				ALOGE("1 Mbps:%llu prev_tput:%llu", Mbps, data[i].prev_tput);
				if (Mbps || data[i].prev_tput)
				{
					/* data size + masking */
					sprintf(data_buf, "0x%x%x\n", Mbps, i + 1);
//#ifdef DEBUF
					ALOGE("2 Mbps:%lu prev_tput:%lu", Mbps, data[i].prev_tput);
//#endif
					write(fd, data_buf, strlen(data_buf) + 1);
				}
				data[i].prev_tput = Mbps;
			}
			data[i].prev = data[i].tmp_sum;
		}

		if (fp)
			fclose(fp);

		sleep(1);
		ALOGE("end of while loop\n");
	}

err1:
	ALOGE("err1 Fail to open node with\n");
	if (fd)
	{
		close(fd);
	}

	return -ENOENT;
}


gboolean timeout_func_cb(gpointer data)
{
	if(gMainLoop)
	{
		ALOGD("Main loop will be terminated.");
		g_main_loop_quit((GMainLoop*)data);
	}
	return FALSE;
}

int main(int argc, char *argv[])
{
	int error, ret = 0;

	// Initialize a GTK main loop
	gMainLoop = g_main_loop_new(NULL, FALSE);
	ALOGE(" Argos started\n");
	printf(" PArgos started\n");


	error = argos_monitor();
	if (error)
	{
		ALOGE("fail to run argos monitor with (%d)\n", error);
	}	

	ALOGE("finish argos monitor\n");

	// Add callbacks to main loop
	g_timeout_add(3, timeout_func_cb, gMainLoop); // Timeout callback: it will be called after 3000ms.

	// Start the main loop of service
	g_main_loop_run(gMainLoop);

	ALOGE("Argos Serice is terminated successfully\n");
	printf("PArgos Serice is terminated successfully\n");

	return ret;
}

//! End of a file
