PREFIX=${PREFIX:-/usr/local}

cp -fP redo $(cat lnks) "$PREFIX/bin"
