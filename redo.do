redo-ifchange cc objfs
redo-ifchange $(cat objfs)

./cc -o "$3" $(cat objfs)
