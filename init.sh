#!/bin/sh

BASEDIR=$(dirname "$0")

FILES="
	breakpoints.cpp
	commands.cpp
	common.h
	cputil.h
	debugger.h
	expr.cpp
	frames.cpp
	jmc.cpp
	main.cpp
	modules.cpp
	modules.h
	native.cpp
	native.h
	native_stubs.cpp
	platform.cpp
	platform.h
	symbolreader.cpp
	symbolreader.h
	torelease.h
	typeprinter.cpp
	typeprinter.h
	valueprint.cpp
	valuewalk.cpp
	valuewalk.h
	varobj.cpp
"

OWNER=i-kulaychuk
REPO=coreclr
TOKEN=9b20b7ffeff3e85174696955d272f2e1bc37678c
REF=debugger

for f in $FILES; do
	LOCATION=src/debug/netcoredbg/$f
	echo "Fetching $LOCATION"
	curl \
		--header "Authorization: token $TOKEN" \
		--header "Accept: application/vnd.github.v3.raw" \
		-o $BASEDIR/$LOCATION \
		https://github.sec.samsung.net/api/v3/repos/$OWNER/$REPO/contents/$LOCATION?ref=$REF
done

LOCATION=src/ToolBox/SOS/NETCore/SymbolReader.cs
echo "Fetching $LOCATION"
curl \
	--header "Authorization: token $TOKEN" \
	--header "Accept: application/vnd.github.v3.raw" \
	-o $BASEDIR/src/debug/netcoredbg/SymbolReader.cs \
	https://github.sec.samsung.net/api/v3/repos/$OWNER/$REPO/contents/$LOCATION?ref=$REF
