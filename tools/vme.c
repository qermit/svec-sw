#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <libvmebus.h>

static char usage_string[] =
	"usage: %s [-v vme_address] "
	"[-d data_width] [-a address_modifier] [-n word_count]\n";

void usage(char *prog)
{
	fprintf(stderr, usage_string, prog);
	exit(1);
}
int main(int argc, char *argv[])
{

	struct vme_mapping map;
	struct vme_mapping *mapp = &map;
	volatile void *ptr;
	unsigned long vmebase, am, data_width;
	int i, count;
	int c;

	/* vme defaults */
	count = 1;
	am = VME_A32_USER_DATA_SCT;
	data_width = VME_D32;

	while ((c = getopt(argc, argv, "v:d:a:n:")) != -1) {
		switch (c) {
		case 'v':
			vmebase = strtoul(optarg, NULL, 0);
			break;
		case 'd':
			data_width = strtoul(optarg, NULL, 0);
			break;
		case 'a':
			am = VME_A32_USER_DATA_SCT;
			break;
		case 'n':
			count = strtoul(optarg, NULL, 0);
			break;
		case '?':
		case 'h':
			usage(argv[0]);
			break;
		default:
			break;
		}
	}

	memset(mapp, 0, sizeof(*mapp));

	mapp->am = 		VME_A32_USER_DATA_SCT;
	mapp->data_width = 	VME_D32;
	mapp->sizel = 		0x80000;
	mapp->vme_addrl =	vmebase;

	if ((ptr = vme_map(mapp, 1)) == (void *)-1) {
		printf("could not map at 0x%08x\n", vmebase);
		exit(1);
	}

	printf("vme 0x%08x kernel 0x%08x user 0x%08x\n", 
			vmebase, mapp->kernel_va, mapp->user_va);
	for (i = 0; i < count; i++, ptr += 4) {
		printf("%08x: %08x\n", ptr, ntohl(*(uint32_t *)ptr));
	}

	vme_unmap(mapp, 1);
	return 0;
}
