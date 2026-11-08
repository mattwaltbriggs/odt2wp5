/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: t; c-basic-offset: 4 -*- */
/* odt2wp5: convert an OpenDocument Text file to WordPerfect.
 *
 *   odt2wp5 input.odt output.wp        # WordPerfect 5.1 (default)
 *   odt2wp5 input.odt output.wp6       # WordPerfect 6.x (by extension)
 *   odt2wp5 input.odt output.wp4       # WordPerfect 4.0 for Mac (by extension)
 *   odt2wp5 --wp4 input.odt output.xyz # WordPerfect 4.0 for Mac (forced)
 *   odt2wp5 --wp6 input.odt output.xyz # WordPerfect 6.x (forced)
 *   odt2wp4 input.odt output.wp        # WordPerfect 4.0 for Mac
 *   odt2wp6 input.odt output.wp        # WordPerfect 6.x
 *
 * The ODT-reading half is shared; only the back-end generator differs. One
 * binary serves all three: when invoked under a name that contains "wp4" or
 * "wp6" (e.g. an odt2wp4 or odt2wp6 symlink), that back-end is the default.
 */

#include "ODTReader.h"
#include "WP4Generator.h"
#include "WP5Generator.h"
#include "WP6Generator.h"

#include <cstdio>
#include <cstring>
#include <memory>

// True if 'name' ends (case-insensitively) with 'suffix'.
static bool hasSuffix(const char *name, const char *suffix)
{
	size_t n = std::strlen(name), s = std::strlen(suffix);
	if (s > n)
		return false;
	for (size_t i = 0; i < s; i++)
	{
		char a = name[n - s + i], b = suffix[i];
		if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
		if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
		if (a != b)
			return false;
	}
	return true;
}

// True if 's' contains 'needle' (case-insensitive, ASCII).
static bool containsCI(const char *s, const char *needle)
{
	size_t nl = std::strlen(needle);
	if (nl == 0)
		return true;
	for (; *s; s++)
	{
		size_t i = 0;
		for (; i < nl; i++)
		{
			char a = s[i], b = needle[i];
			if (a == '\0') return false;
			if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
			if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
			if (a != b) break;
		}
		if (i == nl)
			return true;
	}
	return false;
}

enum Backend { WP4, WP5, WP6 };

int main(int argc, char **argv)
{
	// Default back-end. This is baked in at compile time: odt2wp4 is built
	// with -DODT2WP4_DEFAULT and odt2wp6 with -DODT2WP6_DEFAULT; the plain
	// odt2wp5 binary defaults to WP5.1. Invoking any of them under a name
	// containing "wp4"/"wp6" selects that back-end too (so a renamed copy or
	// symlink still does the right thing). Explicit --wp4/--wp5/--wp6 flags
	// and the output extension still override.
#if defined(ODT2WP4_DEFAULT)
	Backend backend = WP4;
#elif defined(ODT2WP6_DEFAULT)
	Backend backend = WP6;
#else
	Backend backend = WP5;
#endif
	const char *prog = argv[0] ? argv[0] : "odt2wp5";
	const char *base = std::strrchr(prog, '/');
	base = base ? base + 1 : prog;
	if (containsCI(base, "wp4"))
		backend = WP4;
	else if (containsCI(base, "wp6"))
		backend = WP6;

	int argi = 1;
	while (argi < argc && argv[argi][0] == '-' && argv[argi][1] == '-')
	{
		if (std::strcmp(argv[argi], "--wp4") == 0)
			backend = WP4;
		else if (std::strcmp(argv[argi], "--wp5") == 0)
			backend = WP5;
		else if (std::strcmp(argv[argi], "--wp6") == 0)
			backend = WP6;
		else
			break;
		argi++;
	}

	if (argc - argi < 2)
	{
		std::fprintf(stderr,
		             "odt2wp4/odt2wp5/odt2wp6 - convert OpenDocument Text (.odt/.fodt) to WordPerfect\n"
		             "Usage: %s [--wp4|--wp5|--wp6] input.odt output.wp[4|6]\n"
		             "  Back-end defaults to WP4 when invoked as odt2wp4, WP6 as odt2wp6,\n"
		             "  else WP5.1. --wp4/--wp5/--wp6 force a back-end; otherwise an\n"
		             "  output name ending in .wp4 selects WP4, .wp6/.wp61 selects WP6.\n", argv[0]);
		return 1;
	}

	const char *inPath  = argv[argi];
	const char *outPath = argv[argi + 1];

	if (backend == WP5)
	{
		// Extension can still override an explicit/renamed default.
		if (hasSuffix(outPath, ".wp4"))
			backend = WP4;
		else if (hasSuffix(outPath, ".wp6") || hasSuffix(outPath, ".wp61"))
			backend = WP6;
	}

	std::unique_ptr<librevenge::RVNGTextInterface> generator;
	switch (backend)
	{
	case WP4:
		generator.reset(new WP4Generator(outPath));
		break;
	case WP6:
		generator.reset(new WP6Generator(outPath));
		break;
	default:
		generator.reset(new WP5Generator(outPath));
		break;
	}

	ODTReader reader;
	if (!reader.parse(inPath, generator.get()))
	{
		std::fprintf(stderr, "odt2wp5: conversion failed\n");
		return 1;
	}
	std::fprintf(stderr, "odt2wp5: wrote %s (%s)\n", outPath,
	             backend == WP4 ? "WP4.0 Mac" : backend == WP6 ? "WP6" : "WP5.1");
	return 0;
}
