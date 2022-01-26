# dupscan

Quick'n'dirty application to scan a directory tree (Unix-only) and report
duplicate files.

Scan a directory tree (Unix-only) and report duplicate files.
Use the POSIX directory access routines for the scan.
Maintain a linked-list of original files we've already seen.
Use the file size as a first-pass to find an existing entry,
and use a sha256 hash to properly identify two files with the same size.

LIMITATIONS
1. It doesn't like character-special or block-special devices.
2. It also doesn't currently check for hard links.
3. It just ignores symlinks.
4. It could do a better job of storing/finding existing entries, but it
works for me where I'm scanning around 500,000 files.
