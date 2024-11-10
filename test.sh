JPEG=https://jpeg.org/images/jpeg-home.jpg
PNG=https://a3381f52-5e9a-4db6-babe-4d7b4a71b25f.selcdn.net/19.08.24_13-27/bundles/holdingborauto2022/images/fon.png
WEBP=https://www.gstatic.com/webp/gallery/1.webp

rm result/* 2> /dev/null
set -e

./build.sh

enc=jpeg
echo "Testing $enc"

./main $WEBP $enc 
./main $PNG  $enc;

enc=png
echo "Testing $enc"

./main $WEBP $enc 
./main $JPEG $enc;

enc=webp
echo "Testing $enc"

./main $JPEG $enc 
./main $PNG  $enc;

echo
echo "Seems nice"
