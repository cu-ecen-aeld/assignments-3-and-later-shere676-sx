#!/bin/sh

# Check argument count
if [ $# -ne 2 ]
then
    echo "Error: Two arguments required"
    exit 1
fi

filesdir=$1
searchstr=$2

# Verify directory exists
if [ ! -d "$filesdir" ]
then
    echo "Error: $filesdir is not a directory"
    exit 1
fi

# Count files recursively
numfiles=$(find "$filesdir" -type f | wc -l)

# Count matching lines recursively
matchinglines=$(grep -r "$searchstr" "$filesdir" 2>/dev/null | wc -l)

echo "The number of files are $numfiles and the number of matching lines are $matchinglines"