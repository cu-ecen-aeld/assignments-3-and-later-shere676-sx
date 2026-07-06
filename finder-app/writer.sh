#!/bin/sh

# Check argument count
if [ $# -ne 2 ]
then
    echo "Error: Two arguments required"
    exit 1
fi

writefile=$1
writestr=$2

# Create parent directory if needed
mkdir -p "$(dirname "$writefile")"

# Write string to file
if ! echo "$writestr" > "$writefile"
then
    echo "Error: Could not create file"
    exit 1
fi