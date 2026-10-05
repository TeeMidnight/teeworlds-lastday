#!/usr/bin/env python3
import os
import re
import sys

os.chdir(os.path.dirname(__file__) + "/..")

PATH = "src/"
EXCEPTIONS = [
]

MAGNUS = "/* (c) Magnus Auvinen. See license.txt in the root of the distribution for more information. */"
ARCHIVE = "/* (c) Teeworlds Archive Project Contributors."
LASTDAY = "/* (c) Teeworlds LastDay - Bamcane."
MODIFIED = "/* This is a modified version of Teeworlds - see license.txt for details."
FORK_ONLY = "/* (c) Teeworlds Archive Project Contributors. See license.txt. */"

# Any of these is a valid header. Which one a file uses is a one-time decision:
# a file derived from upstream keeps the upstream attribution, a file this
# project wrote on its own needs only the fork attribution. This check makes
# sure every source file carries exactly one of them at the very top.
#
# Deliberately no copyright years: the authoritative notice lives in
# license.txt, so per-file headers never need an annual bump.
HEADERS = (
	[MAGNUS, ARCHIVE, LASTDAY, MODIFIED], # upstream-derived, modified by the fork
	[ARCHIVE, LASTDAY, MODIFIED], # fork-original source file
	[FORK_ONLY], # fork-original, single-line form
)

FORBIDDEN = (
	"licence.txt",
	"acquire a complete release at teeworlds.com",
)

# Per-file headers carry no copyright year on purpose: the authoritative
# notice lives in license.txt, so nothing needs an annual bump.
YEAR = re.compile(r"\(c\) 20\d\d")


def matches(lines, header):
	if len(lines) < len(header):
		return False
	for line, expected in zip(lines, header):
		if not line.startswith(expected):
			return False
	return True


def check_file(filename):
	if filename in EXCEPTIONS:
		return False
	with open(filename) as file:
		lines = [file.readline() for _ in range(4)]
		full = "".join(lines)

	if YEAR.search(full):
		print("Copyright year in per-file license header of {} (keep the year in "
			"license.txt only)".format(filename))
		return True

	if not any(matches(lines, header) for header in HEADERS):
		print("Missing or unrecognized license header in {}".format(filename))
		return True

	for needle in FORBIDDEN:
		if needle in full:
			print("Stale license header text ({!r}) in {}".format(needle, filename))
			return True

	return False


def check_dir(directory):
	errors = 0
	for file in os.listdir(directory):
		path = directory + file
		if os.path.isdir(path):
			if file not in ("external", "generated"):
				errors += check_dir(path + "/")
		elif file.endswith((".c", ".cpp", ".h")):
			errors += check_file(path)
	return errors


if __name__ == '__main__':
	sys.exit(int(check_dir(PATH) != 0))
