#ifndef CIX_LIBDIRS_H
#define CIX_LIBDIRS_H

/*
 * Where libraries live on a Cix host -- one definition (#184).
 *
 * This platform carries FIVE library directories: usr/lib (683 files
 * across 54 packages), lib/x86_64-linux-gnu (483 across 52),
 * usr/lib/x86_64-linux-gnu (135), usr/lib64 (150) and lib64 (11), with
 * 455 basenames existing in two or three of them at once. Most of that
 * is inherited rather than chosen: the multiarch triplet is a Debian
 * convention for letting several architectures share one filesystem,
 * and a Cix image has one architecture.
 *
 * Before this header the layout was asserted in about sixty separate
 * places -- forty of them one hand-maintained list in mkbootroot.c
 * where every entry spelled out its own directory. Moving a single
 * package therefore meant finding and editing all of them, which is
 * why the sprawl had never been reduced: the cost of the first step
 * was the cost of the whole thing.
 *
 * The point of naming it here is that the later steps become ordinary.
 * Stage 4 of #184 flips CIX_LIB_DIR_RUNTIME to "usr/lib" and that is
 * the whole edit on this side; nothing else in the platform spells the
 * directory out any more.
 */

/*
 * The directory the platform PUTS a library in.
 *
 * It is glibc's slibdir, because glibc's compiled-in runtime search
 * path is exactly slibdir + libdir and there is no third slot -- so a
 * library placed anywhere else is found only by accident of one of
 * those two happening to cover it. libdir has been usr/lib since
 * 2.44-7 (#196); when slibdir joins it there, both halves of that
 * search path become the same directory and this constant changes with
 * it, in one place.
 */
#define CIX_LIB_DIR_RUNTIME "lib/x86_64-linux-gnu"

/*
 * The directories the platform LOOKS in, in order.
 *
 * A superset of the above, and deliberately so: what the platform
 * writes is a decision, but what it reads has to cope with whatever a
 * package actually did. During #184's stage 2 the two disagree by
 * design -- a package that has moved to usr/lib is still staged into
 * an image whose other libraries have not -- and this list is what
 * makes that intermediate state work rather than fail.
 *
 * Ordered by likelihood rather than preference: the first hit wins,
 * and a library in more than one of these is the same file in every
 * one of them (455 such basenames today, which is the duplication
 * #184 exists to remove).
 *
 * Used as an initialiser for a `const char *const []`, so it carries
 * its own braces.
 */
#define CIX_LIB_DIRS_SEARCH                                                    \
	{                                                                          \
		"lib/x86_64-linux-gnu", "usr/lib", "lib", "usr/lib/x86_64-linux-gnu",   \
		    "usr/lib64", "lib64"                                               \
	}

/*
 * The directories the platform's OWN library set is staged into and
 * then verified in.
 *
 * The same list as the search path, and that is a correction rather
 * than a coincidence. It used to be the runtime directory plus lib64,
 * which meant the guarantee this pair exists to provide -- that every
 * library the platform built is still byte-for-byte the one it built,
 * because a control-plane root carrying glibc objects from two
 * different builds panics at boot with "Attempted to kill init!", as
 * happened twice on a real machine while assembly reported success
 * both times -- covered only some of the platform's libraries.
 * Measured on 192.168.15.95: cix-hosttools carries 40 shared objects
 * in usr/lib, glibc's own among them, which that pair had never
 * staged or verified. They were never missing from the root (the
 * ordinary image copy puts them there); what they lacked was the
 * last-writer-wins protection, which is the entire point.
 *
 * #184 forced the issue rather than creating it. As packages move to
 * usr/lib they would have dropped out of a set keyed on the old
 * directory one at a time, quietly shrinking the guarantee with every
 * migration -- so the narrower definition was not merely incomplete,
 * it was going to decay.
 *
 * Kept as its own name because the two mean different things: one is
 * "where do I look for a library called X", the other is "which
 * directories hold the set I must protect". They coincide today and
 * there is no reason to expect them to diverge, but a reader of
 * either loop should be told which question it is asking.
 *
 * lib64 is in here for the loader. Its path is fixed by the psABI --
 * PT_INTERP in every binary ever built here -- so unlike the rest of
 * the layout it is not ours to choose, and it stays after #184
 * collapses everything else into one directory.
 */
#define CIX_LIB_DIRS_PLATFORM CIX_LIB_DIRS_SEARCH

#endif /* CIX_LIBDIRS_H */
