src=$(find src -name '*.c')
hdr=$(find src -name '*.h')

redo-ifchange cc $src $hdr

./cc $src -o "$3"
