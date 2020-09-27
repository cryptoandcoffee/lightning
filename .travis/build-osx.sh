#!/bin/bash -x
set -e

CWD=$(pwd)
export SLOW_MACHINE=1
export CC=${COMPILER:-gcc}
export DEVELOPER=${DEVELOPER:-1}
export EXPERIMENTAL_FEATURES=${EXPERIMENTAL_FEATURES:-0}
export COMPAT=${COMPAT:-1}
export PATH=$CWD/dependencies/bin:"$HOME"/.local/bin:"$PATH"
export PYTEST_PAR=4
export PYTEST_SENTRY_ALWAYS_REPORT=1
export BOLTDIR=lightning-rfc
export TEST_DB_PROVIDER=${DB:-"sqlite3"}
export TEST_NETWORK=${NETWORK:-"regtest"}

brew install autoconf automake libtool python3 gmp gnu-sed gettext libsodium wget
sudo ln -s /usr/local/Cellar/gettext/0.20.1/bin/xgettext dependencies/bin
export PATH="/usr/local/opt:$PATH"

brew install sqlite
export LDFLAGS="-L/usr/local/opt/sqlite/lib"
export CPPFLAGS="-I/usr/local/opt/sqlite/include"

brew install pyenv
echo -e 'if command -v pyenv 1>/dev/null 2>&1; then\n  eval "$(pyenv init -)"\nfi' >> ~/.bash_profile
source ~/.bash_profile
pyenv install 3.7.4
pip install --upgrade pip
pyenv local 3.7.4
pip install mako

wget https://bitcoin.org/bin/bitcoin-core-0.20.1/bitcoin-0.20.1-osx64.tar.gz
tar -xjf bitcoin-0.20.1-osx64.tar.gz
mv bitcoin-0.20.1/bin/* dependencies/bin


echo -en 'travis_fold:start:script.build\\r'
./configure
make -j 8
echo -en 'travis_fold:end:script.build\\r'

echo -en 'travis_fold:start:script.test\\r'
$TEST_CMD
echo -en 'travis_fold:end:script.test\\r'

