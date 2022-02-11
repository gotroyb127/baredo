srcf=src/${2#obj/}.c
depsf=src/${2#obj/}.deps

redo-ifchange cc "$srcf" "$depsf"

(
	cd "${srcf%/*}"
	redo-ifchange $(cat "$OLDPWD/$depsf")
)

./cc -c -o "$3" "$srcf"
