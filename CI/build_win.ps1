$COMPILER=$Env:COMPILER
$ARCH=$Env:ARCH

$src_dir=$pwd

mkdir build
cd build

cmake -G "$COMPILER" -A "$ARCH" -DENABLE_IPV6=OFF -DWITH_USB_BACKEND=OFF -DWITH_SERIAL_BACKEND=OFF -DPYTHON_BINDINGS=ON -DLIBXML2_LIBRARIES="$src_dir\deps\lib\libxml2.dll.a" ..
cmake --build . --config Release

ls "$src_dir\build"
ls "$src_dir\build\Release"

mkdir c:\projects\libiio\build-win64
mkdir c:\projects\libiio\build-win64\Release

cp Release\libiio.dll c:\projects\libiio\build-win64\Release\
cp Release\libiio.lib c:\projects\libiio\build-win64\Release\
cp Release\*.exe c:\projects\libiio\build-win64\Release\
cp ..\iio.h c:\projects\libiio\
cp C:\libs\64\*.dll c:\projects\libiio\build-win64\

cp "$src_dir\COPYING.txt" c:\projects\libiio\

ls "$src_dir"

ls c:\projects\libiio\build-win64\
ls c:\projects\libiio\build-win64\Release\

iscc libiio.iss
