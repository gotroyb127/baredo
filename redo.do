src=$(find src -name '*.c')
hdr=$(find src -name '*.h')

redo-ifchange compile $src $hdr

./compile $src -o "$3"
