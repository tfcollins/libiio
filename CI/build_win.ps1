$COMPILER=$Env:COMPILER
$ARCH=$Env:ARCH

$src_dir=$pwd

mkdir build
cd build

#cmake -G "$COMPILER" -A "$ARCH" -DENABLE_IPV6=OFF -DWITH_USB_BACKEND=OFF -DWITH_SERIAL_BACKEND=OFF -DPYTHON_BINDINGS=ON -DLIBXML2_LIBRARIES="$src_dir\deps\lib\libxml2.dll.a" ..
#cmake -G "$COMPILER" -A "$ARCH" -DENABLE_IPV6=OFF -DWITH_USB_BACKEND=OFF -DWITH_SERIAL_BACKEND=OFF -DPYTHON_BINDINGS=ON -DLIBXML2_LIBRARIES="C:\lib\64\libxml2.dll" ..
cmake -G "$COMPILER" -A "$ARCH" -DENABLE_IPV6:BOOL=OFF -DCMAKE_SYSTEM_PREFIX_PATH="C:" -DCSHARP_BINDINGS:BOOL=ON -DPYTHON_BINDINGS:BOOL=OFF -DLIBXML2_LIBRARIES="C:\\libs\\64\\libxml2.lib" -DLIBUSB_LIBRARIES="C:\\libs\\64\\libusb-1.0.lib" -DLIBSERIALPORT_LIBRARIES="C:\\libs\\64\\libserialport.dll.a" ..
cmake --build . --config Release

ls "$src_dir\build"
ls "$src_dir\build\Release"

mkdir c:\projects\libiio\build-win64
mkdir c:\projects\libiio\build-win64\Release
mkdir c:\projects\libiio\build-win64\tests
mkdir c:\projects\libiio\build-win64\tests\Release

cp Release\libiio.dll c:\projects\libiio\build-win64\Release\
cp Release\libiio.lib c:\projects\libiio\build-win64\Release\
cp tests\Release\*.exe c:\projects\libiio\build-win64\tests\Release\
cp ..\iio.h c:\projects\libiio\
cp C:\libs\64\*.dll c:\projects\libiio\build-win64\

cp "$src_dir\COPYING.txt" c:\projects\libiio\

ls "$src_dir"

ls c:\projects\libiio\build-win64\
ls c:\projects\libiio\build-win64\Release\

ls "C:\Program Files (x86)\Microsoft Visual Studio\2019\Enterprise\VC\"
ls "C:\Program Files (x86)\Microsoft Visual Studio\2019\Enterprise\VC\Redist\"

echo "HERE"
find "C:\Program Files (x86)\Microsoft Visual Studio\2019" | grep -i msvcr
echo "HERE2"

ls "C:\Program Files (x86)\Microsoft Visual Studio\2019\Enterprise\VC\Redist\MSVC\"
ls "C:\Program Files (x86)\Microsoft Visual Studio\2019\Enterprise\VC\Redist\MSVC\14.29.30133\x64\"
ls "C:\Program Files (x86)\Microsoft Visual Studio\2019\Enterprise\VC\Redist\MSVC\14.29.30133\x64\Microsoft.VC142.CRT\"


iscc libiio.iss

cp C:\libiio-setup.exe "$src_dir\build"
