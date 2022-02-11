redo-ifchange srcfs

while read -r srcf
do
	printf '%s\n' "obj/${srcf%.c}.o"
done < srcfs
