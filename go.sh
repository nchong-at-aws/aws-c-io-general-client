PREFIX=`pwd`
git clone https://github.com/awslabs/aws-c-common.git
mkdir -p aws-c-common/build
pushd aws-c-common/build
cmake -DCMAKE_INSTALL_PREFIX=$PREFIX/install ..
make -j && make install
popd
git clone https://github.com/awslabs/aws-c-io.git
mkdir -p aws-c-io/build
pushd aws-c-io/build
cmake -DCMAKE_INSTALL_PREFIX=$PREFIX/install -DCMAKE_PREFIX_PATH=$PREFIX/install ..
make -j && make install
make test
