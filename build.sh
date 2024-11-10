set -e
W="-Wall"

main () {
    UNAVAILABLE_H=unavailable_b64.h
    echo '#define UNAVAILABLE_B64_EXT "jpeg"' > $UNAVAILABLE_H
    echo -n '#define UNAVAILABLE_B64 "' >> $UNAVAILABLE_H
    B64=$(base64 -i unavailable.jpeg)
    echo -n $B64 >> $UNAVAILABLE_H
    echo -n '"' >> $UNAVAILABLE_H
    set -x
    gcc main.c b64/encode.c b64/buffer.c -Llib -lcurl -Iinclude -Iinclude/webp -lz -lm -lpng -ljpeg -lwebp -lsharpyuv -g -o main $W $@
}

curl() {
    set -x
    gcc curl.c -lcurl -o curl $W $1
}

main $@
