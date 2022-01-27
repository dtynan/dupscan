/*
 * Copyright (c) 2022, Dermot Tynan.  All rights reserved.
 *
 * This is free software; you can redistribute it and/or modify it under the
 * terms of the GNU General Public License as published by the Free Software
 * Foundation; either version 2, or (at your option) any later version.
 *
 * It is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this product; see the file COPYING.  If not, write to the Free
 * Software Foundation, 675 Mass Ave, Cambridge, MA 02139, USA.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR "AS IS" AND ANY EXPRESS
 * OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT,
 * INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
 * STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING
 * IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 * ABSTRACT
 * Scan a directory tree (Unix-only) and report duplicate files. Use the
 * POSIX directory access routines for the scan. Maintain a linked-list of
 * original files we've already seen. Use the file size as a first-pass
 * to find an existing entry, and use a sha256 hash to properly identify
 * two files with the same size.
 *
 * LIMITATIONS
 * 1. It doesn't like character-special or block-special devices.
 * 2. It also doesn't currently check for hard links.
 * 3. It just ignores symlinks.
 * 4. It could do a better job of storing/finding existing entries,
 *    but it works for me where I'm scanning around 500,000 files.
 */
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/types.h>
#include <dirent.h>
#include <sys/stat.h>
#include <string.h>

#define HASH_SIZE	1049

#ifdef __FreeBSD__
#  define HASH_COMMAND	"sha256"
#else
#  define HASH_COMMAND	"sha256sum"
#endif

/*
 * Structure for maintaining list of already-seen, original entries.
 * Keep the dev/ino pair so we can look for hard links (in the
 * future). We keep the size as a quick test. Obviously, two files
 * cannot be the same if they have different sizes. So, start with
 * that parameter.
 */
struct	entry	{
	struct entry	*next;
	char		*path;
	size_t		size;
	char		*hash;
	nlink_t		nlinks;
	dev_t		device;
	ino_t		inode;
};

/*
 * Some basic variables. Verbose is used to increase the amount of
 * chat/output. no_effect allows for a dry-run, as eventually this will
 * replace the file with a symlink to the original.
 */
int		verbose;
int		no_effect;
struct entry	*entry_list[HASH_SIZE];
struct entry	*freelist = NULL;

/*
 * Prototypes.
 */
void		scan_dups(char *);
void		process(char *, char *);
void		regular_file(struct entry *);
void		generate_hash(struct entry *);
struct entry	*find_entry(struct entry *);
struct entry	*entry_alloc(char *);
void		entry_free(struct entry *);
void		usage();

/*
 * All life begins here...
 */
int
main(int argc, char *argv[])
{
	int i;

	opterr = verbose = no_effect = 0;
	while ((i = getopt(argc, argv, "nv")) != EOF) {
		switch (i) {
		case 'n':
			/*
			 * "Claytons" mode. Don't do anything harmful
			 * to the system - just report what might
			 * have happened.
			 */
			no_effect = 1;
			break;

		case 'v':
			/*
			 * Be chatty.
			 */
			verbose = 1;
			break;

		default:
			usage();
			break;
		}
	}
	if ((argc - optind) != 1)
		usage();
	for (i = 0; i < HASH_SIZE; i++)
		entry_list[i] = NULL;
	scan_dups(argv[optind]);
	exit(0);
}

/*
 * Scan a directory recursively, and build a tree of unique entries.
 */
void
scan_dups(char *path)
{
	DIR *dirp;
	struct dirent *dp;

	if (verbose)
		printf("Directory: %s\n", path);
	if ((dirp = opendir(path)) == NULL) {
		perror(path);
		exit(1);
	}
	while ((dp = readdir(dirp)) != NULL) {
		if (*dp->d_name == '.' && (dp->d_name[1] == '\0' || strcmp(dp->d_name, "..") == 0))
			continue;
		process(path, dp->d_name);
	}
	closedir(dirp);
}

/*
 * Process a single file or directory. We're looking for duplications.
 * First check the file size against our "database" of file sizes
 * and hashes. If the file size is identical, then check the file
 * hash (generating it if needed.
 */
void
process(char *path, char *name)
{
	char *cp;
	struct entry *ep;
	struct stat stbuf;

	/*
	 * Allocate space for the fully-qualified path.
	 */
	if ((cp = (char *)malloc(strlen(path) + strlen(name) + 2)) == NULL) {
		perror("process malloc");
		exit(1);
	}
	strcpy(cp, path);
	strcat(cp, "/");
	strcat(cp, name);
	if (lstat(cp, &stbuf) < 0) {
		perror("process lstat");
		exit(1);
	}
	switch (stbuf.st_mode & S_IFMT) {
	case S_IFREG:
		/*
		 * A regular file - ignore zero-length files. I
		 * don't care about them. For everything else, store
		 * what we've gleaned and call the regular file
		 * function to see if there's a duplicate.
		 */
		if (stbuf.st_size > 0L) {
			ep = entry_alloc(cp);
			ep->size = stbuf.st_size;
			ep->nlinks = stbuf.st_nlink;
			ep->device = stbuf.st_dev;
			ep->inode = stbuf.st_ino;
			regular_file(ep);
		}
		break;

	case S_IFDIR:
		/*
		 * A directory - magic recursion!
		 */
		scan_dups(cp);
		break;

	case S_IFLNK:
		/*
		 * Not sure what to do about symlinks, but for the
		 * most part, they're inert, so I'm ignoring them.
		 */
		if (verbose)
			printf("Ignoring a symlink (%s).\n", cp);
		break;

	default:
		/*
		 * Must be a character-special or block-special
		 * device. Not sure how that happened, in this day
		 * and age. We must have crossed into /dev or
		 * something. Either way, stop now before we do
		 * real damage.
		 */
		fprintf(stderr, "Can't handle file type for %s.\n", cp);
		exit(1);
	}
}

/*
 * We have a regular file - is it a duplicate?
 *
 * Where a duplicate file is found, perform some sort of action.
 * This would normally be to create a symlink back to the original.
 * A nicer optimization might be to just record the duplicate, and
 * then mark the actual directory as a duplicate if all the files
 * in the directory are duplicates. Then, just remove all the files
 * in a duplicate directory (recursively). Finally, symlink the
 * directory or the individual files back to their originals. One
 * "gotcha" with that approach is cross-links in two dirs.
 */
void
regular_file(struct entry *ep)
{
	struct entry *dup_ep;

	if (verbose)
		printf("Regular file: %s, size: %ld.\n", ep->path, ep->size);
	if ((dup_ep = find_entry(ep)) != NULL) {
		printf(">>> DUP file: %s. ", ep->path);
		printf("Original: %s.\n", dup_ep->path);
		entry_free(ep);
	}
}

/*
 * Find an existing entry, based on the current (passed-in) entry. Returns
 * the original entry if one already exists.
 */
struct entry *
find_entry(struct entry *orig_ep)
{
	int hash;
	struct entry *ep, *last_ep;

	hash = orig_ep->size % HASH_SIZE;
	if (verbose)
		printf("Search for file: %s (size:%ld,hash%d).\n", orig_ep->path, orig_ep->size, hash);
	if (entry_list[hash] == NULL || entry_list[hash]->size > orig_ep->size) {
		orig_ep->next = entry_list[hash];
		entry_list[hash] = orig_ep;
		return(NULL);
	}
	for (last_ep = NULL, ep = entry_list[hash]; ep != NULL && ep->size <= orig_ep->size; ep = ep->next) {
		if (ep->size == orig_ep->size) {
			/*
			 * Size match!
			 */
			if (verbose)
				printf("Matches (size) for %s.\n", ep->path);
			/*
			 * We do a "lazy-load" of the hash entry.
			 * In other words, only hash the file(s)
			 * if there is a size match. In a perfect
			 * world, all the files would have different
			 * sizes, and we'd have no need to compute
			 * a hash. So, in the optimistic hope that
			 * this is the case, we delay the hash
			 * computation until we need it.
			 */
			if (orig_ep->hash == NULL)
				generate_hash(orig_ep);
			if (ep->hash == NULL)
				generate_hash(ep);
			if (strcmp(ep->hash, orig_ep->hash) == 0) {
				if (verbose)
					printf("Matches (hash).\n");
				return(ep);
			}
		}
		last_ep = ep;
	}
	/*
	 * No match for the file. Need to add it in the current position.
	 */
	orig_ep->next = last_ep->next;
	last_ep->next = orig_ep;
	return(NULL);
}

/*
 * Generate a cryptographically secure (no collisions) hash of the file.
 */
void
generate_hash(struct entry *ep)
{
	int cmdlen;
	char *cp, *cmd;
	FILE *pp;

	cmdlen = strlen(HASH_COMMAND) + strlen(ep->path) + 100;
	if ((cmd = (char *)malloc(cmdlen)) == NULL) {
		perror("generate_hash malloc");
		exit(1);
	}
	sprintf(cmd, "%s \"%s\"", HASH_COMMAND, ep->path);
	if ((pp = popen(cmd, "r")) == NULL) {
		fprintf(stderr, "generate_hash: ");
		perror(cmd);
		exit(1);
	}
	if ((cp = fgets(cmd, cmdlen - 1, pp)) == NULL) {
		fprintf(stderr, "Some sort of hash failure?!? Do you have permission to access the file?\n");
		fprintf(stderr, "File: %s\n", ep->path);
		perror("System reports");
		exit(1);
	}
	pclose(pp);
	if ((cp = strchr(cmd, ' ')) != NULL)
		*cp = '\0';
	ep->hash = cmd;
}

/*
 * Allocate a new entry and set some basics, like the full path.
 */
struct entry *
entry_alloc(char *name)
{
	struct entry *ep;

	if ((ep = freelist) != NULL) {
		freelist = ep->next;
	} else {
		if ((ep = (struct entry *)malloc(sizeof(*ep))) == NULL) {
			perror("entry_alloc");
			exit(1);
		}
	}
	ep->next = NULL;
	ep->path = name;
	ep->size = 0L;
	ep->hash = NULL;
	return(ep);
}

/*
 * Release an entry back to the freelist.
 */
void
entry_free(struct entry *ep)
{
	if (ep->path != NULL)
		free((void *)ep->path);
	if (ep->hash != NULL)
		free((void *)ep->hash);
	ep->path = ep->hash = NULL;
	ep->next = freelist;
	freelist = ep;
}

/*
 * Print a usage message and exit.
 */
void
usage()
{
	fprintf(stderr, "Usage: dupscan [-nv] <dir>\n");
	exit(2);
}
