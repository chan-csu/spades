#!/bin/sh

if [ -z "$1" ]
then
	echo Usage: $0 database.fasta
	echo Example: $0 SSURef_108_NR_tax_silva_v2.fasta
	exit
fi


tempfile=`basename $1`.tmp
indxfile=`basename $1`

echo Building taxonomy index for $1 ...
echo Temporary file: $tempfile
echo Index prefix: $indxfile

python preprocessing.py < $1 > $tempfile

bowtie-build $tempfile $indxfile

rm $tempfile