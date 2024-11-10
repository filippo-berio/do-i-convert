set -e
W="-Wall"

main () {
    THIRD_PARTY="b64/encode.c b64/buffer.c lib/libjpeg.a lib/libwebp.a lib/libsharpyuv.a lib/libpng.a"
    UNAVAILABLE_H=unavailable_b64.h
    echo '#define UNAVAILABLE_B64_EXT "jpeg"' > $UNAVAILABLE_H
    echo -n '#define UNAVAILABLE_B64 "' >> $UNAVAILABLE_H
    B64=$(base64 -i unavailable.jpeg)
    echo -n $B64 >> $UNAVAILABLE_H
    echo -n '"' >> $UNAVAILABLE_H
    set -x
    gcc main.c $THIRD_PARTY -lcurl -Iinclude -Iinclude/webp -lz -lm -g -o main $W $@
}

curl() {
    set -x
    gcc curl.c -lcurl -o curl $W $1
}

main $@
