// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2000 Silicon Graphics, Inc.
 * All Rights Reserved.
 */
/*
 * iogen - a tool for generating file/sds io for a doio process
 */

#include "global.h"

#ifdef HAVE_SYS_SYSSGI_H
#include <sys/syssgi.h>
#endif

#ifdef HAVE_SYS_UUID_H
#include <sys/uuid.h>
#endif

#ifdef HAVE_SYS_FS_XFS_FSOPS_H
#include <sys/fs/xfs_fsops.h>
#endif

#ifdef HAVE_SYS_FS_XFS_ITABLE_H
#include <sys/fs/xfs_itable.h>
#endif


#include "doio.h"
#include "str_to_bytes.h"
#include "string_to_tokens.h"
#include "open_flags.h"
#include "random_range.h"

#ifndef BSIZE
#define BSIZE 512
#endif

#define RAW_IO(_flags_)	((_flags_) & (O_RAW | O_SSD))

#define SYSERR	strerror(errno)

/*
 * Structure for retaining test file information
 */

struct file_info {
	char	f_path[MAX_FNAME_LENGTH+1]; /* file name (full path)	*/
	int	f_length;	/* length in bytes	    	    	*/
	int	f_iou;		/* file iounit  	    	    	*/
	int	f_riou;		/* file raw iounit (for O_RAW/O_SSD)    */
	int	f_dalign;	/* direct I/O alignment                 */
	int	f_nextoff;	/* offset of end of last io operation   */
	int	f_type; 	/* file type S_IFREG, etc...		*/
	int	f_lastoffset;   /* offset of last io operation  	*/
	int	f_lastlength;   /* length of last io operation   	*/
};

/*
 * Simple structure for associating strings with values - useful for converting
 * cmdline args to internal values, as well as printing internal values in
 * a human readable form.
 */

struct strmap {
	char	*m_string;
	int	m_value;
	int	m_flags;
};

/*
 * Declare cmdline option flags/variables initialized in parse_cmdline()
 */

#define OPTS	"a:dhf:i:L:m:op:qr:s:t:T:O:N:"

int	a_opt = 0;		/* async io comp. types supplied	    */
int 	o_opt = 0;		/* form overlapping requests	    	    */
int 	f_opt = 0;		/* test flags   	    	    	    */
int 	i_opt = 0;		/* iterations - 0 implies infinite	    */
int	L_opt = 0;		/* listio min-max nstrides & nents	    */
int 	m_opt = 0;		/* offset mode	    	    	    	    */
int	O_opt = 0;		/* file creation Open flags		    */
int 	p_opt = 0;		/* output pipe - default is stdout	    */
int 	r_opt = 0;		/* specify raw io multiple instead of	    */
				/* getting it from the mounted on device.   */
				/* Only applies to regular files.   	    */
int 	s_opt = 0;		/* syscalls	    	    	    	    */
int 	t_opt = 0;		/* min transfer size (bytes)    	    */
int 	T_opt = 0;		/* max transfer size (bytes)    	    */
int 	q_opt = 0;		/* quiet operation on startup   	    */
char	TagName[40];		/* name of this iogen (see Monster)	    */
struct	strmap *Offset_Mode;	/* M_SEQUENTIAL, M_RANDOM, etc. 	    */
int 	Iterations;		/* # requests to generate (0 --> infinite)  */
int 	Time_Mode = 0;		/* non-zero if Iterations is in seconds	    */
				/* (ie. -i arg was suffixed with 's')       */
char	*Outpipe;		/* Pipe to write output to if p_opt 	    */
int 	Mintrans;		/* min io transfer size	    	    	    */
int 	Maxtrans;		/* max io transfer size	    	    	    */
int 	Rawmult;		/* raw/ssd io multiple (from -r)    	    */
int	Minstrides;		/* min # of listio strides per request	    */
int	Maxstrides;		/* max # of listio strides per request	    */
int	Oflags;			/* open(2) flags for creating files	    */
int	Ocbits;			/* open(2) cbits for creating files	    */
int	Ocblks;			/* open(2) cblks for creating files	    */
int	Orealtime=0;		/* flag set for -O REALTIME		    */
int	Oextsize=0;		/* real-time extent size		    */
int	Oreserve=1;		/* flag for -O [no]reserve		    */
int	Oallocate=0;		/* flag for -O allocate			    */
int	Owrite=1;		/* flag for -O nowrite			    */

int	Nfiles = 0;		/* # files on cmdline			    */
struct	file_info *File_List;	/* info about each file	    		    */
int	Nflags = 0;		/* # flags on cmdline			    */
struct	strmap *Flag_List[128];	/* flags selected from cmdline		    */
int	Nsyscalls = 0;		/* # syscalls on cmdline		    */
struct	strmap *Syscall_List[128]; /* syscalls selected on cmdline	    */
int	Fileio = 0;		/* flag indicating that a file		    */
				/* io syscall has been chosen.	 	    */
int	Naio_Strat_Types = 0;	/* # async io completion types		    */
struct	strmap *Aio_Strat_List[128]; /* Async io completion types	    */

void	startup_info(FILE *stream, int seed);

/*
 * Map async io completion modes (-a args) names to values.  Macros are
 * defined in doio.h.
 */

struct strmap	Aio_Strat_Map[] = {
#ifndef __linux__
	{ "poll",	A_POLL		},
	{ "signal",	A_SIGNAL	},
#else
	{ "none",	0	},
#endif /* !linux */
	{ NULL,		-1		}
};

/*
 * Offset_Mode #defines
 */

#define M_RANDOM    	1
#define M_SEQUENTIAL	2
#define M_REVERSE   	3

/*
 * Map offset mode (-m args) names to values
 */

struct strmap	Omode_Map[] = {
	{ "random",		M_RANDOM	},
	{ "sequential",		M_SEQUENTIAL    },
	{ "reverse",		M_REVERSE   	},
	{ NULL,			-1	    	}
};

/*
 * Map syscall names (-s args) to values - macros are defined in doio.h.
 */
#define	SY_ASYNC	00001
#define	SY_WRITE	00002
#define	SY_SDS		00010
#define	SY_LISTIO	00020
#define	SY_NENT		00100	/* multi entry vs multi stride >>> */

struct strmap	Syscall_Map[] = {
	{ "read",		READ,		0			},
	{ "write",		WRITE,		SY_WRITE		},
	{ "pread",		PREAD   				},
	{ "pwrite",		PWRITE,		SY_WRITE		},
	{ "resvsp",		RESVSP, 	SY_WRITE		},
	{ "unresvsp",		UNRESVSP, 	SY_WRITE		},
	{ "reserve",		RESVSP, 	SY_WRITE		},
	{ "unreserve",		UNRESVSP, 	SY_WRITE		},
	{ "readv",		READV					},
	{ "writev",		WRITEV,		SY_WRITE		},
	{ "mmread",		MMAPR					},
	{ "mmwrite",		MMAPW,		SY_WRITE		},
	{ "fsync2",		FSYNC2, 	SY_WRITE		},
	{ "fdatasync",		FDATASYNC, 	SY_WRITE		},
	{ NULL,			-1      }
};

/*
 * Map open flags (-f args) to values
 */
#define	FLG_RAW		00001

struct strmap	Flag_Map[] = {
	{ "buffered",		0,			0	},
	{ "sync",		O_SYNC,			0	},
	{ "direct",		O_DIRECT,		FLG_RAW },
	{ NULL, 	    -1	    	}
};

/*
 * Map file types to strings
 */

struct strmap	Ftype_Map[] = {
	{ "regular",	S_IFREG },
	{ "blk-spec",	S_IFBLK },
	{ "chr-spec",	S_IFCHR },
	{ NULL,		0 }
};

/*
 * Misc declarations
 */

int		Sds_Avail;

char	Byte_Patterns[26] = { 'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H',
			      'I', 'J', 'K', 'L', 'M', 'N', 'O', 'P',
			      'Q', 'R', 'S', 'T', 'U', 'V', 'W', 'X',
			      'Y', 'Z' };


int form_iorequest(struct io_req *);
int init_output();
int parse_cmdline(int argc, char **argv, char *opts);
int help(FILE *stream);
int usage(FILE *stream);


int
main(argc, argv)
int 	argc;
char	**argv;
{
    int	    	    rseed, outfd, infinite;
    time_t  	    start_time;
    struct io_req   req;
    
    umask(0);
    Sds_Avail = 0;
    TagName[0] = '\0';
    parse_cmdline(argc, argv, OPTS);

    /*
     * Initialize output descriptor.  
     */
    if (! p_opt) {
	outfd = 1;
    } else {
	outfd = init_output();
    }

    rseed = getpid();
    random_range_seed(rseed);       /* initialize random number generator */

    /*
     * Print out startup information, unless we're running in quiet mode
     */
    if (!q_opt)
	startup_info(stderr, rseed);

    start_time = time(0);

    /*
     * While iterations (or forever if Iterations == 0) - compute an
     * io request, and write the structure to the output descriptor
     */

    infinite = !Iterations;

    while (infinite ||
	   (! Time_Mode && Iterations--) ||
	   (Time_Mode && time(0) - start_time <= Iterations)) {

	memset(&req, 0, sizeof(struct io_req));
	if (form_iorequest(&req) == -1) {
	    fprintf(stderr, "iogen%s:  form_iorequest() failed\n", TagName);
	    continue;
	}

	req.r_magic = DOIO_MAGIC;
	write(outfd, (char *)&req, sizeof(req));
    }

    exit(0);

}   /* main */

void
startup_info(FILE *stream, int seed)
{
    char	*value_to_string(struct strmap *map, int val), *type;
    int		i;

    fprintf(stream, "\n");
    fprintf(stream, "iogen%s starting up with the following:\n", TagName);
    fprintf(stream, "\n");

    fprintf(stream, "Out-pipe:              %s\n", 
	    p_opt ? Outpipe : "stdout");

    if (Iterations) {
	fprintf(stream, "Iterations:            %d", Iterations);
	if (Time_Mode)
	    fprintf(stream, " seconds");

	fprintf(stream, "\n");
    } else {
	fprintf(stream, "Iterations:            Infinite\n");
    }

    fprintf(stream,
	    "Seed:                  %d\n", seed);

    fprintf(stream,
	    "Offset-Mode:           %s\n", Offset_Mode->m_string);

    fprintf(stream, "Overlap Flag:          %s\n",
	    o_opt ? "on" : "off");

    fprintf(stream,
	    "Mintrans:              %-11d (%d blocks)\n",
	    Mintrans, (Mintrans+BSIZE-1)/BSIZE);

    fprintf(stream,
	    "Maxtrans:              %-11d (%d blocks)\n",
	    Maxtrans, (Maxtrans+BSIZE-1)/BSIZE);

    if (! r_opt) 
	fprintf(stream,
		"O_RAW/O_SSD Multiple:  (Determined by device)\n");
    else
	fprintf(stream,
		"O_RAW/O_SSD Multiple:  %-11d (%d blocks)\n",
		Rawmult, (Rawmult+BSIZE-1)/BSIZE);

    fprintf(stream, "Syscalls:              ");
    for (i = 0; i < Nsyscalls; i++)
	fprintf(stream,
		"%s ", Syscall_List[i]->m_string);
    fprintf(stream, "\n");

    fprintf(stream, "Aio completion types:  ");
    for (i = 0; i < Naio_Strat_Types; i++)
	fprintf(stream,
		"%s ", Aio_Strat_List[i]->m_string);
    fprintf(stream, "\n");

    if (Fileio) {
	fprintf(stream, "Flags:                 ");
	for (i = 0; i < Nflags; i++)
	    fprintf(stream,
		    "%s ", Flag_List[i]->m_string);

	fprintf(stream, "\n");
	fprintf(stream, "\n");
	fprintf(stream, "Test Files:  \n");
	fprintf(stream, "\n");
	fprintf(stream,
		"Path                                          Length    iou   raw iou file\n");
	fprintf(stream,
		"                                              (bytes) (bytes) (bytes) type\n"); 
	fprintf(stream,
		"-----------------------------------------------------------------------------\n");

	for (i = 0; i < Nfiles; i++) {
	    type = value_to_string(Ftype_Map, File_List[i].f_type);
	    fprintf(stream, "%-40s %12d %7d %7d %s\n",
		    File_List[i].f_path, File_List[i].f_length,
		    File_List[i].f_iou, File_List[i].f_riou, type);
	}
    }
}

/*
 * Initialize output descriptor.  If going to stdout, its easy,
 * otherwise, attempt to create a FIFO on path Outpipe.  Exit with an
 * error code if this cannot be done.
 */
int
init_output()
{
    int	 outfd;
    struct stat	    sbuf;

    if (stat(Outpipe, &sbuf) == -1) {
	if (errno == ENOENT) {
	    if (mkfifo(Outpipe, 0666) == -1) {
		fprintf(stderr, "iogen%s:  Could not mkfifo %s:  %s\n",
			TagName, Outpipe, SYSERR);
		exit(2);
	    }
	} else {
	    fprintf(stderr, "iogen%s:  Could not stat outpipe %s:  %s\n",
		    TagName, Outpipe, SYSERR);
	    exit(2);
	}
    } else {
	if (! S_ISFIFO(sbuf.st_mode)) {
	    fprintf(stderr,
		    "iogen%s:  Output file %s exists, but is not a FIFO\n",
		    TagName, Outpipe);
	    exit(2);
	}
    }

    if ((outfd = open(Outpipe, O_RDWR)) == -1) {
	fprintf(stderr,
		"iogen%s:  Couldn't open outpipe %s with flags O_RDWR: %s\n",
		TagName, Outpipe, SYSERR);
	exit(2);
    }

    return(outfd);
}


/*
 * Main io generation function.  form_iorequest() selects a system call to
 * do based on cmdline arguments, and proceeds to select parameters for that
 * system call.
 *
 * Returns 0 if req is filled in with a complete doio request, otherwise
 * returns -1.
 */

int
form_iorequest(req)
struct io_req   *req;
{
    int	    	    	mult, offset = 0, length = 0, slength;
    int	    	    	minlength, maxlength, laststart, lastend;
    int	    	    	minoffset, maxoffset;
    int			maxstride, nstrides;
    char    	    	pattern, *errp;
    struct strmap	*flags, *sc, *aio_strat;
    struct file_info	*fptr;

    /*
     * Choose system call, flags, and file
     */

    sc = Syscall_List[random_range(0, Nsyscalls-1, 1, NULL)];
    req->r_type = sc->m_value;

    if( sc->m_flags & SY_WRITE )
	    pattern = Byte_Patterns[random_range(0, sizeof(Byte_Patterns) - 1, 1, NULL)];
    else
	    pattern = 0;

    /*
     * otherwise, we're doing file io.  Choose starting offset, length,
     * open flags, and possibly a pattern (for write/writea).
     */

    fptr = &File_List[random_range(0, Nfiles-1, 1, NULL)];
    flags = Flag_List[random_range(0, Nflags-1, 1, NULL)];

    /*
     * Choose offset/length multiple.  IO going to a device, or regular
     * IO that is O_RAW or O_SSD must be aligned on the file r_iou.  Otherwise
     * it must be aligned on the regular iou (normally 1).
     */

    if ( fptr->f_type == S_IFREG && (flags->m_flags & FLG_RAW) )
	    mult = fptr->f_riou;
    else
	    mult = fptr->f_iou;

    /*
     * Choose offset and length.  Both must be a multiple of mult
     */

    /*
     * Choose length first - it must be a multiple of mult
     */

    laststart = fptr->f_lastoffset;
    lastend = fptr->f_lastoffset + fptr->f_lastlength - 1;

    minlength = (Mintrans > mult) ? Mintrans : mult;

    switch (Offset_Mode->m_value) {
    case M_SEQUENTIAL:
	if (o_opt && lastend > laststart)
	    offset = random_range(laststart, lastend, 1, NULL);
	else
	    offset = lastend + 1;
 	if (offset && (offset%mult))
	    offset += mult - (offset % mult);

	if (minlength > fptr->f_length - offset)
	    offset = 0;

	maxlength = fptr->f_length - offset;
	if (maxlength > Maxtrans)
	    maxlength = Maxtrans;

	length = random_range(minlength, maxlength, mult, &errp);
	if (errp != NULL) {
	    fprintf(stderr, "iogen%s:  random_range(%d, %d, %d) failed\n",
		    TagName, minlength, maxlength, mult);
	    return -1;
	}

	break;

    case M_REVERSE:
	maxlength = laststart;

	if (maxlength > Maxtrans)
	    maxlength = Maxtrans;

	if (minlength > maxlength) {
	    laststart = fptr->f_length;
	    lastend = fptr->f_length;
	    maxlength = Maxtrans;
	}

	length = random_range(minlength, maxlength, mult, &errp);
	if (errp != NULL) {
	    fprintf(stderr, "iogen%s:  random_range(%d, %d, %d) failed\n",
		    TagName, minlength, maxlength, mult);
	    return -1;
	}

	offset = laststart - length;

	if (o_opt && lastend > laststart)
	    offset += random_range(1, lastend - laststart, 1, NULL);

	if (offset &&  (offset%mult))
	    offset -= offset % mult;

	break;
    
    case M_RANDOM:
	length = random_range(Mintrans, Maxtrans, mult, NULL);

	if (o_opt && lastend > laststart) {
		minoffset = laststart - length + 1;
		if (minoffset < 0) {
			minoffset = 0;
		}

		if (lastend + length > fptr->f_length) {
			maxoffset = fptr->f_length - length;
		} else {
			maxoffset = lastend;
		}
	} else {
	    minoffset = 0;
	    maxoffset = fptr->f_length - length;
	}

	if (minoffset < 0)
	    minoffset = 0;

	offset = random_range(minoffset, maxoffset, mult, &errp);
	if (errp != NULL) {
	    fprintf(stderr, "iogen%s:  random_range(%d, %d, %d) failed\n",
		    TagName, minoffset, maxoffset, mult);
	    return -1;
	}
    }

    fptr->f_lastoffset = offset;
    fptr->f_lastlength = length;

    /*
     * Choose an async io completion strategy if necessary
     */
    if( sc->m_flags & SY_ASYNC )
	    aio_strat = Aio_Strat_List[random_range(0, Naio_Strat_Types - 1,
						    1, NULL)];
    else
	    aio_strat = NULL;

    /*
     * fill in specific syscall record data
     */
    switch (sc->m_value) {
    case READ:
    case READA:
	strcpy(req->r_data.read.r_file, fptr->f_path);
	req->r_data.read.r_oflags = O_RDONLY | flags->m_value;
	req->r_data.read.r_offset = offset;
	req->r_data.read.r_nbytes = length;
	req->r_data.read.r_uflags = (flags->m_flags & FLG_RAW) ? F_WORD_ALIGNED : 0;
	req->r_data.read.r_aio_strat = (aio_strat==NULL) ? 0 : aio_strat->m_value;
	req->r_data.read.r_nstrides = 1;
	req->r_data.read.r_nent = 1;
	break;

    case WRITE:
    case WRITEA:
	strcpy(req->r_data.write.r_file, fptr->f_path);
	req->r_data.write.r_oflags = O_WRONLY | flags->m_value;
	req->r_data.write.r_offset = offset;
	req->r_data.write.r_nbytes = length;
	req->r_data.write.r_pattern = pattern;
	req->r_data.write.r_uflags = (flags->m_flags & FLG_RAW) ? F_WORD_ALIGNED : 0;
	req->r_data.write.r_aio_strat = (aio_strat==NULL) ? 0 : aio_strat->m_value;
	req->r_data.write.r_nstrides = 1;
	req->r_data.write.r_nent = 1;
	break;

    case READV:
    case AREAD:
    case PREAD:
    case WRITEV:
    case AWRITE:
    case PWRITE:

    case LREAD:
    case LREADA:
    case LWRITE:
    case LWRITEA:

    case RESVSP:
    case UNRESVSP:
    case FSYNC2:
    case FDATASYNC:

	strcpy(req->r_data.io.r_file, fptr->f_path);
	req->r_data.io.r_oflags = ((sc->m_flags & SY_WRITE) ? O_WRONLY : O_RDONLY) | flags->m_value;
	req->r_data.io.r_offset = offset;
	req->r_data.io.r_nbytes = length;
	req->r_data.io.r_pattern = pattern;
	req->r_data.io.r_uflags = (flags->m_flags & FLG_RAW) ? F_WORD_ALIGNED : 0;
	req->r_data.io.r_aio_strat = (aio_strat==NULL) ? 0 : aio_strat->m_value;
	req->r_data.io.r_nstrides = 1;
	req->r_data.io.r_nent = 1;
	break;

    case MMAPR:
    case MMAPW:
	strcpy(req->r_data.io.r_file, fptr->f_path);
	/* a subtle "feature" of mmap: a write-map requires
           the file open read/write */
	req->r_data.io.r_oflags = ((sc->m_flags & SY_WRITE) ? O_RDWR : O_RDONLY) | flags->m_value;
	req->r_data.io.r_offset = offset;
	req->r_data.io.r_nbytes = length;
	req->r_data.io.r_pattern = pattern;
	req->r_data.io.r_uflags = (flags->m_flags & FLG_RAW) ? F_WORD_ALIGNED : 0;
	req->r_data.io.r_aio_strat = (aio_strat==NULL) ? 0 : aio_strat->m_value;
	req->r_data.io.r_nstrides = 1;
	req->r_data.io.r_nent = 1;
	break;

    case LSREAD:
    case LSREADA:
    case LEREAD:
    case LEREADA:
    case LSWRITE:
    case LSWRITEA:
    case LEWRITE:
    case LEWRITEA:
	/* multi-strided */
	strcpy(req->r_data.io.r_file, fptr->f_path);
	req->r_data.io.r_oflags = ((sc->m_flags & SY_WRITE) ? O_WRONLY : O_RDONLY) | flags->m_value;
	req->r_data.io.r_offset = offset;
	req->r_data.io.r_uflags = (flags->m_flags & FLG_RAW) ? F_WORD_ALIGNED : 0;
	req->r_data.io.r_aio_strat = (aio_strat==NULL) ? 0 : aio_strat->m_value;
	req->r_data.io.r_pattern = pattern;

	/* multi-strided request...
	 *  random number of strides (1...MaxStrides)
	 *  length of stride must be > minlength
	 *  length of stride must be % mult
	 *
	 * maxstrides = min(length / mult, overall.max#strides)
	 * nstrides = random #
	 * while( length / nstrides < minlength )
	 *	nstrides = new random #
	 */
	maxstride = length / mult;
	if(maxstride > Maxstrides)
	    maxstride = Maxstrides;

	if(!Minstrides)
		Minstrides=1;
	nstrides = random_range(Minstrides, maxstride, 1, &errp);
	if (errp != NULL) {
	    fprintf(stderr, "iogen%s:  random_range(%d, %d, %d) failed\n",
		    TagName, Minstrides, maxstride, 1);
	    return -1;
	}

	slength = length / nstrides;
	if(slength % mult != 0) {
	    if( mult > slength) {
		slength = mult;
	    } else {
		slength -= slength % mult;
	    }
	    nstrides = length / slength;
	    if(nstrides > Maxstrides)
		    nstrides = Maxstrides;
	}

	req->r_data.io.r_nbytes = slength;
	if( sc->m_flags & SY_NENT ) {
		req->r_data.io.r_nstrides = 1;
		req->r_data.io.r_nent = nstrides;
	} else {
		req->r_data.io.r_nstrides = nstrides;
		req->r_data.io.r_nent = 1;
	}
	break;

    case LISTIO:
	break;
    }

    return 0;
}

/*
 * Get information about a file that iogen uses to choose io length and
 * offset.  Information gathered is file length, iounit, and raw iounit.
 * For regurlar files, iounit is 1, and raw iounit is the iounit of the
 * device on which the file resides.  For block/character special files
 * the iounit and raw iounit are both the iounit of the device.
 *
 * Note:	buffered and osync io must be iounit aligned
 *		raw and ossd io must be raw iounit aligned
 */

int
get_file_info(rec)
struct file_info    *rec;
{
    struct stat			sbuf;
    int				fd;
    struct dioattr		finfo;

    /*
     * Figure out if the files is regular, block or character special.  Any
     * other type is an error.
     */

    if (stat(rec->f_path, &sbuf) == -1) {
	fprintf(stderr, "iogen%s: get_file_info():  Could not stat() %s:  %s\n",
		TagName, rec->f_path, SYSERR);
	return -1;
    }

    rec->f_type = sbuf.st_mode & S_IFMT;

    /*
     * If regular, iou is 1, and we must figure out the device on
     * which the file resides.  riou is the iou (logical sector size) of
     * this device.
     */

    if (S_ISREG(sbuf.st_mode)) {
	rec->f_iou = 1;
	rec->f_length = sbuf.st_size;

	/*
	 * If -r used, take Rawmult as the raw/ssd io multiple.  Otherwise
	 * attempt to determine it by looking at the device the file
	 * resides on.
	 */

	if (r_opt) {
	    rec->f_riou = Rawmult;
	    return 0;
	}

	rec->f_riou = BSIZE;
	if( (fd = open(rec->f_path, O_RDWR|O_DIRECT, 0)) != -1 ) {
	    char *dio_env;
#ifdef XFS_IOC_DIOINFO
	    if(xfsctl(rec->f_path, fd, XFS_IOC_DIOINFO, &finfo) != -1) {
#else
#ifdef F_DIOINFO
	    if(fcntl(fd, F_DIOINFO, &finfo) != -1) {
#else
bozo!
#endif
#endif
		dio_env = getenv("XFS_DIO_MIN");
		if (dio_env)
		    finfo.d_mem = finfo.d_miniosz = atoi(dio_env);

		rec->f_riou = finfo.d_miniosz;
	    } else {
		fprintf(stderr,
			"iogen%s: Error %s (%d) getting direct I/O info of file %s\n",
			TagName, strerror(errno), errno, rec->f_path);
	    }
	    close(fd);
	} else {
	    rec->f_riou = BBSIZE;
	}
    } else {

	rec->f_riou = BSIZE;
	rec->f_length = BSIZE;
    }

    return 0;
}

/*
 * Create file path as nbytes long.  If path exists, the file will either be
 * extended or truncated to be nbytes long.  Returns final size of file,
 * or -1 if there was a failure.
 */

int
create_file(path, nbytes)
char	*path;
int 	nbytes;
{
    int	    	fd, rval;
    char    	c;
    struct stat	sbuf;
    int		nb;
    struct flock64 f;
    struct fsxattr xattr;
    struct dioattr finfo;
    char	*b, *buf;

    errno = 0;
    rval = stat(path, &sbuf);

    if (rval == -1) {
	if (errno == ENOENT) {
	    sbuf.st_size = 0;
	} else {
	    fprintf(stderr, "iogen%s:  Could not stat file %s:  %s (%d)\n",
		    TagName, path, SYSERR, errno);
	    return -1;
	}
    } else {
	if (! S_ISREG(sbuf.st_mode)) {
	    fprintf(stderr,
		    "iogen%s:  file %s exists, but is not a regular file - cannot modify length\n",
		    TagName, path);
	    return -1;
	}
    }

    if (sbuf.st_size == nbytes)
	return nbytes;

    Oflags |= O_CREAT | O_WRONLY;

    if ((fd = open(path, Oflags, 0666)) == -1) {
	fprintf(stderr, "iogen%s:  Could not create/open file %s: %s (%d)\n",
		TagName, path, SYSERR, errno);
	return -1;
    }

    /*
     * Truncate file if it is longer than nbytes, otherwise attempt to
     * pre-allocate file blocks.
     */

    if (sbuf.st_size > nbytes) {
	if (ftruncate(fd, nbytes) == -1) {
	    fprintf(stderr,
		    "iogen%s:  Could not ftruncate() %s to %d bytes:  %s (%d)\n",
		    TagName, path, nbytes, SYSERR, errno);
	    close(fd);
	    return -1;
	}
    } else {

	/*
	 *  The file must be designated as Real-Time before any data
	 *  is allocated to it.
	 *
	 */
	if(Orealtime != 0) {
	    bzero(&xattr, sizeof(xattr));
	    xattr.fsx_xflags = XFS_XFLAG_REALTIME;
	    /*fprintf(stderr, "set: fsx_xflags = 0x%x\n", xattr.fsx_xflags);*/
#ifdef XFS_IOC_FSSETXATTR
	    if( xfsctl(path, fd, XFS_IOC_FSSETXATTR, &xattr) == -1 ) {
#else
#ifdef F_FSSETXATTR
		    if (fcntl(fd, F_FSSETXATTR, &xattr) < 0) {
#else
bozo!
#endif
#endif
		fprintf(stderr, "iogen%s: Error %s (%d) setting XFS XATTR->Realtime on file %s\n",
			TagName, SYSERR, errno, path);
		close(fd);
		return -1;
	    }

#ifdef DEBUG
#ifdef XFS_IOC_FSGETXATTR
	    if( xfsctl(path, fd, XFS_IOC_FSGETXATTR, &xattr) == -1 ) {
#else
#ifdef F_FSGETXATTR
	    if (fcntl(fd, F_FSGETXATTR, &xattr) < 0) {
#else
bozo!
#endif
#endif
		fprintf(stderr, "iogen%s: Error getting realtime flag %s (%d)\n",
			TagName, SYSERR, errno);
		close(fd);
		return -1;
	    } else {
		fprintf(stderr, "get: fsx_xflags = 0x%x\n", 
			xattr.fsx_xflags);
	    }
	}

	/*
	 * Reserve space
	 *
	 * Failure is ignored since XFS_IOC_RESVSP only works on XFS and the
	 * filesystem could be on some otehr filesystem.
	 */
	if( Oreserve ) {
	    f.l_whence = SEEK_SET;
	    f.l_start = 0;
	    f.l_len = nbytes;

	    /*fprintf(stderr,
		    "create_file: xfsctl(%d, RESVSP, { %d, %lld, %lld })\n",
		   fd, f.l_whence, (long long)f.l_start, (long long)f.l_len);*/

	    /* non-zeroing reservation */
#if defined(FALLOCATE)
	    if (fallocate(fd, FALLOC_FL_KEEP_SIZE, 0, f.l_len) == -1) {
		fprintf(stderr,
			"iogen%s:  Could not fallocate %d bytes in file %s: %s (%d)\n",
			TagName, nbytes, path, SYSERR, errno);
		close(fd);
		return -1;
	    }
#elif defined(XFS_IOC_RESVSP)
	    if( xfsctl( path, fd, XFS_IOC_RESVSP, &f ) == -1) {
		fprintf(stderr,
			"iogen%s:  Could not xfsctl(XFS_IOC_RESVSP) %d bytes in file %s: %s (%d)\n",
			TagName, nbytes, path, SYSERR, errno);
		close(fd);
		return -1;
	    }
#elif defined(F_RESVSP)
	    if( fcntl( fd, F_RESVSP, &f ) == -1) {
		fprintf(stderr,
			"iogen%s:  Could not fcntl(F_RESVSP) %d bytes in file %s: %s (%d)\n",
			TagName, nbytes, path, SYSERR, errno);
		close(fd);
		return -1;
	    }
#else
# error Dont know how to reserve space!
#endif
	}

	if( Oallocate ) {
	    /* XFS_IOC_ALLOCSP allocates from the file start to l_start */
	    f.l_whence = SEEK_SET;
	    f.l_start = nbytes;
	    f.l_len = 0;
	    /*fprintf(stderr,
		    "create_file: xfsctl(%d, XFS_IOC_ALLOCSP, { %d, %lld, %lld })\n",
		    fd, f.l_whence, (long long)f.l_start,
		    (long long)f.l_len);*/

	    /* zeroing reservation */
#if defined(FALLOCATE)
	    if (fallocate(fd, 0, sbuf.st_size, nbytes - sbuf.st_size) == -1) {
		fprintf(stderr,
			"iogen%s:  Could not fallocate %d bytes in file %s: %s (%d)\n",
			TagName, nbytes, path, SYSERR, errno);
		close(fd);
		return -1;
	    }
#elif defined(XFS_IOC_ALLOCSP)
	    if( xfsctl( path, fd, XFS_IOC_ALLOCSP, &f ) == -1) {
		fprintf(stderr,
			"iogen%s:  Could not xfsctl(XFS_IOC_ALLOCSP) %d bytes in file %s: %s (%d)\n",
			TagName, nbytes, path, SYSERR, errno);
		close(fd);
		return -1;
	    }
#elif defined(F_ALLOCSP)
	    if ( fcntl(fd, F_ALLOCSP, &f) < 0) {
		fprintf(stderr,
			"iogen%s:  Could not fcntl(F_ALLOCSP) %d bytes in file %s: %s (%d)\n",
			TagName, nbytes, path, SYSERR, errno);
		close(fd);
		return -1;
	    }
#else
# error Dont know how to (pre)allocate space!
#endif
	}
#endif

	/*
	 * Write a byte at the end of file so that stat() sets the right
	 * file size.
	 */

	if(Owrite == 2) {
	    close(fd);
	    if( (fd = open(path, O_CREAT|O_RDWR|O_DIRECT, 0)) != -1 ) {
		char *dio_env;
#ifdef XFS_IOC_DIOINFO
		if(xfsctl(path, fd, XFS_IOC_DIOINFO, &finfo) == -1) {
#else
#ifdef F_DIOINFO
	        if (fcntl(fd, F_DIOINFO, &finfo) < 0) {
#else
bozo!
#endif
#endif
		    fprintf(stderr,
			    "iogen%s: Error %s (%d) getting direct I/O info for file %s\n",
			    TagName, SYSERR, errno, path);
		    return -1;
		} else {
		    /*fprintf(stderr, "%s: miniosz=%d\n", 
			    path, finfo.d_miniosz);*/
		}

		dio_env = getenv("XFS_DIO_MIN");
		if (dio_env)
			finfo.d_mem = finfo.d_miniosz = atoi(dio_env);
	    } else {
		fprintf(stderr, "iogen%s: Error %s (%d) opening file %s with flags O_CREAT|O_RDWR|O_DIRECT\n",
			TagName, SYSERR, errno, path);
		return -1;
	    }

	    /*
	     * nb is nbytes adjusted down by an even d_miniosz block
	     *
	     * Note: the first adjustment can cause iogen to print a warning 
	     * 	about not being able to create a file of <nbytes> length, 
	     *	since the file will be shorter.
	     */
	    nb = nbytes-finfo.d_miniosz;
	    nb = nb-nb%finfo.d_miniosz;

	    /*fprintf(stderr,
		    "create_file_ow2: lseek(%d, %d {%d %d}, SEEK_SET)\n",
		    fd, nb, nbytes, finfo.d_miniosz);*/

	    if (lseek(fd, nb, SEEK_SET) == -1) {
		fprintf(stderr,
			"iogen%s:  Could not lseek() to EOF of file %s: %s (%d)\n\tactual offset %d file size goal %d miniosz %lld\n",
			TagName, path, SYSERR, errno,
			nb, nbytes, (long long)finfo.d_miniosz);
		close(fd);
		return -1;
	    }

	    b = buf = (char *)malloc(finfo.d_miniosz+finfo.d_mem);

	    if( ((long)buf % finfo.d_mem != 0) ) {
		buf += finfo.d_mem - ((long)buf % finfo.d_mem);
	    }
	    
	    memset(buf, 0, finfo.d_miniosz);

	    if ( (rval=write(fd, buf, finfo.d_miniosz)) != finfo.d_miniosz) {
		fprintf(stderr,
			"iogen%s:  Could not write %d byte length file %s: %s (%d)\n",
			TagName, nb, path, SYSERR, errno);
		fprintf(stderr,
			"\twrite(%d, 0x%lx, %d) = %d\n",
			fd, (long)buf, finfo.d_miniosz, rval);
		fprintf(stderr,
			"\toffset %d file size goal %d, miniosz=%d\n",
			nb, nbytes, finfo.d_miniosz);
		close(fd);
		return -1;
	    }
	    free(b);
	} else
	    if(Owrite) {
	    /*fprintf(stderr,
		    "create_file_Owrite: lseek(%d, %d {%d}, SEEK_SET)\n",
		    fd, nbytes-1, nbytes);*/

	    if (lseek(fd, nbytes-1, SEEK_SET) == -1) {
		fprintf(stderr,
			"iogen%s:  Could not lseek() to EOF in file %s:  %s (%d)\n\toffset goal %d\n",
			TagName, path, SYSERR, errno,
			nbytes-1);
		close(fd);
		return -1;
	    }

	    if ( (rval=write(fd, &c, 1)) != 1) {
		fprintf(stderr,
			"iogen%s:  Could not create a %d byte length file %s: %s (%d)\n",
			TagName, nbytes, path, SYSERR, errno);
		fprintf(stderr,
			"\twrite(%d, 0x%lx, %d) = %d\n",
			fd, (long)&c, 1, rval);
		fprintf(stderr,
			"\toffset %d file size goal %d\n",
			nbytes-1, nbytes);
		close(fd);
		return -1;
	    }
	}
    }

    fstat(fd, &sbuf);
    close(fd);

    return sbuf.st_size;
}

/*
 * Function to convert a string to its corresponding value in a strmap array.
 * If the string is not found in the array, the value corresponding to the
 * NULL string (the last element in the array) is returned.
 */

int
str_to_value(map, str)
struct strmap	*map;
char	    	*str;
{
    struct strmap   *mp;

    for (mp = map; mp->m_string != NULL; mp++)
	if (strcmp(mp->m_string, str) == 0)
	    break;

    return mp->m_value;
}
    
/*
 * Function to convert a string to its corresponding entry in a strmap array.
 * If the string is not found in the array, a NULL is returned.
 */

struct strmap *
str_lookup(map, str)
struct strmap	*map;
char	    	*str;
{
    struct strmap   *mp;

    for (mp = map; mp->m_string != NULL; mp++)
	if (strcmp(mp->m_string, str) == 0)
	    break;

    return((mp->m_string == NULL) ? NULL : mp);
}
    

/*
 * Function to convert a value to its corresponding string in a strmap array.
 * If the value is not found in the array, NULL is returned.
 */

char *
value_to_string(map, val)
struct strmap   *map;
int		val;
{
    struct strmap  *mp;

    for (mp = map; mp->m_string != NULL; mp++)
	if (mp->m_value == val)
	    break;

    return mp->m_string;
}

/*
 * Interpret cmdline options/arguments.  Exit with 1 if something on the
 * cmdline isn't kosher.
 */

int
parse_cmdline(argc, argv, opts)
int 	argc;
char	**argv;
char	*opts;
{
    int	    	    	o, len, nb, format_error;
    struct strmap	*flgs, *sc;
    char    	    	*file, *cp, ch;
    struct strmap   	*mp;
    struct file_info	*fptr;
    int			nopenargs;
    char		*openargs[5];	/* Flags, cbits, cblks */
    while ((o = getopt(argc, argv, opts)) != EOF) {
        switch ((char)o) {

 	case 'a':
	    fprintf(stderr, "iogen%s:  Unrecognized option -a on this platform\n", TagName);
	    exit(2);
	    break;

 	case 'f':
	    cp = strtok(optarg, ",");
	    while (cp != NULL) {
		if( (flgs = str_lookup(Flag_Map, cp)) == NULL ) {
		    fprintf(stderr, "iogen%s:  Unrecognized flags:  %s\n", TagName, cp);
		    exit(2);
		}

		cp = strtok(NULL, ",");

#ifdef O_SSD
		if (flgs->m_value & O_SSD && ! Sds_Avail) {
		    fprintf(stderr, "iogen%s:  Warning - no sds available, ignoring ssd flag\n", TagName);
		    continue;
		}
#endif

		Flag_List[Nflags++] = flgs;
	    }
	    f_opt++;
	    break;

	case 'h':
	    help(stdout);
	    exit(0);
	    break;

	case 'i':
	    format_error = 0;

	    switch (sscanf(optarg, "%i%c", &Iterations, &ch)) {
	    case 1:
		Time_Mode = 0;
		break;

	    case 2:
		if (ch == 's')
		    Time_Mode = 1;
		else
		    format_error = 1;
		break;

	    default:
		format_error = 1;
	    }

	    if (Iterations < 0)
		format_error = 1;

	    if (format_error) {
		fprintf(stderr, "iogen%s:  Illegal -i arg (%s):  Must be of the format:  number[s]\n", TagName, optarg);
		fprintf(stderr, "        where 'number' is >= 0\n");
		exit(1);
	    }

	    i_opt++;
	    break;

	case 'L':
	    fprintf(stderr, "iogen%s:  Unrecognized option -L on this platform\n", TagName);
	    exit(2);
	    break;

	case 'm':
	    if ((Offset_Mode = str_lookup(Omode_Map, optarg)) == NULL) {
		fprintf(stderr, "iogen%s:  Illegal -m arg (%s)\n", TagName, optarg);
		exit(1);
	    }

	    m_opt++;
	    break;

	case 'N':
	    sprintf( TagName, "(%.37s)", optarg );
	    break;

	case 'o':
	    o_opt++;
	    break;

	case 'O':

	    nopenargs = string_to_tokens(optarg, openargs, 4, ":/");
	    if(!strcmp(openargs[0], "realtime")) {
		/*
		 * -O realtime:extsize
		 */
		Orealtime = 1;
		if(nopenargs > 1)
		    sscanf(openargs[1],"%i", &Oextsize);
		else
		    Oextsize=0;
	    } else if( !strcmp(openargs[0], "allocate") ||
		       !strcmp(openargs[0], "allocsp")) {
		/*
		 * -O allocate
		 */
		Oreserve=0;
		Oallocate=1;
	    } else if(!strcmp(openargs[0], "reserve")) {
		/*
		 * -O [no]reserve
		 */
		Oallocate=0;
		Oreserve=1;
	    } else if(!strcmp(openargs[0], "noreserve")) {
		/* Oreserve=1 by default; this clears that default */
		Oreserve=0;
	    } else if(!strcmp(openargs[0], "nowrite")) {
		/* Owrite=1 by default; this clears that default */
		Owrite=0;
	    } else if(!strcmp(openargs[0], "direct")) {
		/* this means "use direct i/o to preallocate file" */
		Owrite=2;
	    } else {
		fprintf(stderr, "iogen%s: Error: -O %s error: unrecognized option\n",
			TagName, openargs[0]);
		exit(1);
	    }
	    O_opt++;
	    break;

	case 'p':
	    Outpipe = optarg;
	    p_opt++;
	    break;

	case 'r':
	    if ((Rawmult = str_to_bytes(optarg)) == -1 ||
		          Rawmult < 11 || Rawmult % BSIZE) {
		fprintf(stderr, "iogen%s:  Illegal -r arg (%s).  Must be > 0 and multipe of BSIZE (%d)\n",
			TagName, optarg, BSIZE);
		exit(1);
	    }

	    r_opt++;
	    break;

 	case 's':
	    cp = strtok(optarg, ",");
	    while (cp != NULL) {
		if ((sc = str_lookup(Syscall_Map, cp)) == NULL) {
		    fprintf(stderr, "iogen%s:  Unrecognized syscall:  %s\n", TagName, cp);
		    exit(2);
		}

		do {
		    /* >>> sc->m_flags & FLG_SDS */
		    if (sc->m_value != SSREAD && sc->m_value != SSWRITE)
			Fileio++;

		    Syscall_List[Nsyscalls++] = sc;
		} while ( (sc = str_lookup(++sc, cp)) != NULL);

		cp = strtok(NULL, ",");
	    }
	    s_opt++;
	    break;

	case 't':
	    if ((Mintrans = str_to_bytes(optarg)) == -1) {
		fprintf(stderr, "iogen%s:  Illegal -t arg (%s):  Must have the form num[bkm]\n", TagName, optarg);
		exit(1);
	    }
	    t_opt++;
	    break;

	case 'T':
	    if ((Maxtrans = str_to_bytes(optarg)) == -1) {
		fprintf(stderr, "iogen%s:  Illegal -T arg (%s):  Must have the form num[bkm]\n", TagName, optarg);
		exit(1);
	    }
	    T_opt++;
	    break;

	case 'q':
	    q_opt++;
	    break;

	case '?':
	    usage(stderr);
	    exit(1);
	}
    }

    /*
     * Supply defaults
     */

    if( ! L_opt ) {
	    Minstrides = 1;
	    Maxstrides = 255;
    }

    if (! m_opt)
	Offset_Mode = str_lookup(Omode_Map, "sequential");

    if (! i_opt)
	Iterations = 0;

    if (! t_opt)
	Mintrans = 1;

    if (! T_opt)
	Maxtrans = 256 * BSIZE;

    if( ! O_opt) 
	Oflags = Ocbits = Ocblks = 0;

    /*
     * Supply default async io completion strategy types.
     */

    if (! a_opt) {
	    for (mp = Aio_Strat_Map; mp->m_string != NULL; mp++) {
		    Aio_Strat_List[Naio_Strat_Types++] = mp;
	    }
    }

    /*
     * Supply default syscalls.  Default is read,write,reada,writea,listio.
     */

    if (! s_opt) {
	Nsyscalls = 0;
	Syscall_List[Nsyscalls++] = str_lookup(Syscall_Map, "read");
	Syscall_List[Nsyscalls++] = str_lookup(Syscall_Map, "write");
	Syscall_List[Nsyscalls++] = str_lookup(Syscall_Map, "pread");
	Syscall_List[Nsyscalls++] = str_lookup(Syscall_Map, "pwrite");
	Syscall_List[Nsyscalls++] = str_lookup(Syscall_Map, "readv");
	Syscall_List[Nsyscalls++] = str_lookup(Syscall_Map, "writev");
	Syscall_List[Nsyscalls++] = str_lookup(Syscall_Map, "mmread");
	Syscall_List[Nsyscalls++] = str_lookup(Syscall_Map, "mmwrite");

        Fileio = 1;
    }

    if (Fileio && (argc - optind < 1)) {
	fprintf(stderr, "iogen%s:  No files specified on the cmdline\n", TagName);
	exit(1);
    }

    /*
     * Supply default file io flags - defaut is 'buffered,raw,sync,ldraw'.
     */

    if (! f_opt && Fileio) {
	    Nflags = 0;
	    Flag_List[Nflags++] = str_lookup(Flag_Map, "buffered");
	    Flag_List[Nflags++] = str_lookup(Flag_Map, "sync");
    }

    if (Fileio) {
	if (optind >= argc) {
	    fprintf(stderr, "iogen%s:  No files listed on the cmdline\n", TagName);
	    exit(1);
	}

	/*
	 * Initialize File_List[] - only necessary if doing file io.  First 
	 * space for the File_List array, then fill it in.
	 */
	
	File_List = (struct file_info *)
	    malloc((argc-optind) * sizeof(struct file_info));
	
	if (File_List == NULL) {
	    fprintf(stderr, "iogen%s:  Could not malloc space for %d file_info structures\n", TagName, argc-optind);
	    exit(2);
	}

	memset(File_List, 0, (argc-optind) * sizeof(struct file_info));

        Nfiles = 0;
        while (optind < argc) {
   	    len = -1;

	    /*
	     * Pick off leading len: if it's there and create/extend/trunc
	     * the file to the desired length.  Otherwise, just make sure
	     * the file is accessable.
	     */

	    if ((cp = strchr(argv[optind], ':')) != NULL) {
	        *cp = '\0';
	        if ((len = str_to_bytes(argv[optind])) == -1) {
		    fprintf(stderr,
			    "iogen%s:  illegal file length (%s) for file %s\n",
			    TagName, argv[optind], cp+1);
		    exit(2);
	        }
	        *cp = ':';
	        file = cp+1;

		if (strlen(file) > MAX_FNAME_LENGTH) {
		    fprintf(stderr, "iogen%s:  Max fname length is %d chars - ignoring file %s\n",
			    TagName, MAX_FNAME_LENGTH, file);
		    optind++;
		    continue;
		}

		nb = create_file(file, len);

		if (nb < len) {
		    fprintf(stderr,
			    "iogen%s warning:  Couldn't create file %s of %d bytes\n",
			    TagName, file, len);

		    if (nb <= 0) {
			optind++;
			continue;
		    }
		}
	    } else {
	        file = argv[optind];
	        if (access(file, R_OK | W_OK) == -1) {
	   	    fprintf(stderr, "iogen%s:  file %s cannot be accessed for reading and/or writing:  %s (%d)\n",
			    TagName, file, SYSERR, errno);
		    exit(2);
	        }
	    }

	    /*
	     * get per-file information
	     */

	    fptr = &File_List[Nfiles];

	    if (file[0] == '/') {
	        strcpy(fptr->f_path, file);
	    } else {
	        getcwd(fptr->f_path,
		       sizeof(fptr->f_path)-1);
	        strcat(fptr->f_path, "/");
	        strcat(fptr->f_path, file);
	    }

	    if (get_file_info(fptr) == -1) {
	        fprintf(stderr, "iogen%s warning:  Error getting file info for %s\n", TagName, file);
	    } else {

		/*
		 * If the file length is smaller than our min transfer size,
		 * ignore it.
		 */

		if (fptr->f_length < Mintrans) {
		    fprintf(stderr, "iogen%s warning:  Ignoring file %s\n",
			    TagName, fptr->f_path);
		    fprintf(stderr, "                length (%d) is < min transfer size (%d)\n",
			    fptr->f_length, Mintrans);
		    optind++;
		    continue;
		}

                /*
                 * If the file length is smaller than our max transfer size,
                 * ignore it.
                 */

                if (fptr->f_length < Maxtrans) {
                    fprintf(stderr, "iogen%s warning:  Ignoring file %s\n",
                            TagName, fptr->f_path);
                    fprintf(stderr, "                length (%d) is < max transfer size (%d)\n",
                            fptr->f_length, Maxtrans);
                    optind++;
                    continue;
                }

		if (fptr->f_length > 0) {
		    switch (Offset_Mode->m_value) {
		    case M_SEQUENTIAL:
			fptr->f_lastoffset = 0;
			fptr->f_lastlength = 0;
			break;
			
		    case M_REVERSE:
			fptr->f_lastoffset = fptr->f_length;
			fptr->f_lastlength = 0;
			break;
			
		    case M_RANDOM:
			fptr->f_lastoffset = fptr->f_length / 2;
			fptr->f_lastlength = 0;
			break;
		    }
		    
		    Nfiles++;
		}
	    }

 	    optind++;
	}

        if (Nfiles == 0) {
	    fprintf(stderr, "iogen%s:  Could not create, or gather info for any test files\n", TagName);
	    exit(2);
        }
    }

    return 0;
}

int
help(stream)
FILE	*stream;
{
    usage(stream);
    fprintf(stream, "\n");
#ifndef __linux__
    fprintf(stream, "\t-a aio_type,...  Async io completion types to choose.  Supported types\n");
    fprintf(stream, "\t                 are:  poll, signal, suspend, and callback.\n");
    fprintf(stream, "\t                 Default is all of the above.\n");
#else /* !linux */
    fprintf(stream, "\t-a               (Not used on Linux).\n");
#endif /* !linux */
    fprintf(stream, "\t-f flag,...      Flags to use for file IO.  Supported flags are\n");
    fprintf(stream, "\t                 buffered, direct, sync.\n");
    fprintf(stream, "\t                 Default is 'buffered,sync'.\n");
    fprintf(stream, "\t-h               This help.\n");
    fprintf(stream, "\t-i iterations[s] # of requests to generate.  0 means causes iogen\n");
    fprintf(stream, "\t                 to run until it's killed.  If iterations is suffixed\n");
    fprintf(stream, "\t                 with 's', then iterations is the number of seconds\n");
    fprintf(stream, "\t                 that iogen should run for.  Default is '0'.\n");
    fprintf(stream, "\t-L min:max       listio nstrides / nrequests range\n");
    fprintf(stream, "\t-m offset-mode   The mode by which iogen chooses the offset for\n");
    fprintf(stream, "\t                 consectutive transfers within a given file.\n");
    fprintf(stream, "\t                 Allowed values are 'random', 'sequential',\n");
    fprintf(stream, "\t                 and 'reverse'.\n");
    fprintf(stream, "\t                 sequential is the default.\n");
    fprintf(stream, "\t-N tagname       Tag name, for Monster.\n");
    fprintf(stream, "\t-o               Form overlapping consecutive requests.\n");
    fprintf(stream, "\t-O               Open flags for creating files\n");
    fprintf(stream, "\t                 realtime:extsize - put file on real-time volume\n");
    fprintf(stream, "\t                 allocate - allocate space with F_ALLOCSP\n");
    fprintf(stream, "\t                 reserve - reserve space with F_RESVSP (default)\n");
    fprintf(stream, "\t                 noreserve - do not reserve with F_RESVSP\n");
    fprintf(stream, "\t                 direct - use O_DIRECT I/O to write to the file\n");
    fprintf(stream, "\t-p               Output pipe.  Default is stdout.\n");
    fprintf(stream, "\t-q               Quiet mode.  Normally iogen spits out info\n");
    fprintf(stream, "\t                 about test files, options, etc. before starting.\n");
    fprintf(stream, "\t-s syscall,...   Syscalls to do.  Supported syscalls are\n");
#ifdef __linux__
    fprintf(stream, "\t                 read, write, pread, pwrite, readv, writev,\n");
    fprintf(stream, "\t                 mmread, mmwrite, fsync2, fdatasync,\n");
    fprintf(stream, "\t                 Default is 'read,write,readv,writev,mmread,mmwrite'.\n");
#endif
    fprintf(stream, "\t-t mintrans      Min transfer length\n");
    fprintf(stream, "\t-T maxtrans      Max transfer length\n");
    fprintf(stream, "\n");
    fprintf(stream, "\t[len:]file,...   Test files to do IO against (note ssread/sswrite\n");
    fprintf(stream, "\t                 don't need a test file).  The len: syntax\n");
    fprintf(stream, "\t                 informs iogen to first create/expand/truncate the\n");
    fprintf(stream, "\t                 to the desired length.\n");
    fprintf(stream, "\n");
    fprintf(stream, "\tNote:  The ssd flag causes sds transfers to also be done.\n");
    fprintf(stream, "\t       To totally eliminate sds transfers, you must eleminate sds\n");
    fprintf(stream, "\t       from the flags (-f) and ssread,ssrite from the syscalls (-s)\n");
    fprintf(stream, "\tThe mintrans, maxtrans, and len: parameters are numbers of the\n");
    fprintf(stream, "\tform [0-9]+[bkm].  The optional trailing b, k, or m multiplies\n");
    fprintf(stream, "\tthe number by blocks, kilobytes, or megabytes.  If no trailing\n");
    fprintf(stream, "\tmultiplier is present, the number is interpreted as bytes\n");

    return 0;
}

/*
 * Obvious - usage clause
 */

int
usage(stream)
FILE	*stream;
{
    fprintf(stream, "usage%s:  iogen [-hoq] [-a aio_type,...] [-f flag[,flag...]] [-i iterations] [-p outpipe] [-m offset-mode] [-s syscall[,syscall...]] [-t mintrans] [-T maxtrans] [ -O file-create-flags ] [[len:]file ...]\n", TagName);
    return 0;
}
