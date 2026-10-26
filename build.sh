#!/bin/bash
#
# build.sh
#
# Must be run as root.  build script

PROJECT_DIR=$(pwd)
EXEC="iboot-agent"
OUTPUT_DIR="$PROJECT_DIR/bin"
OUTPUT_BIN="$PROJECT_DIR/$EXEC"
TEST_BIN=$(which $EXEC)
CONFIG_FILE="/etc/iboot-agent/agent.conf"
COMMAND_ARG=$1



remove_exec_from_path() {
    local executable=$1
    local dir
    local candidate
    local found=0

    for dir in ${PATH//:/ }; do
        candidate="$dir/$executable"
        if [ -e "$candidate" ]; then
            found=1
            echo -e "[  DELETED  ]\t\t$candidate"
            rm -f "$candidate"
        fi
    done

    if [ $found -eq 0 ]; then
        echo -e "[  NOT FOUND  ]\t\t$executable"
    fi
}

if [ "$(id -u)" -ne 0 ]; then
    echo "error: must be run as root" >&2
    exit 1
fi

remove_exec_from_path "$EXEC"

pushd "$PROJECT_DIR" > /dev/null || exit 1
echo -e "[  CLEANING ]\t\t$PROJECT_DIR"
make clean > /dev/null 2>&1
SUCCESS=$?
if [ $SUCCESS -ne 0 ]; then
    echo -e "[  CLEANING ]\t\terror: cleaning failed" >&2
    popd > /dev/null
    exit 1
else
    echo -e "[  CLEANING ]\t\tOK"
fi


sleep 1

if [ -e "$OUTPUT_BIN" ]; then
    echo -e "[  DELETED  ]\t\t$OUTPUT_BIN"
    rm -f "$OUTPUT_BIN" > /dev/null 2>&1
fi

echo -e "[  BUILDING ]\t\t$EXEC"
make -j4 > /dev/null 2>&1
SUCCESS=$?
if [ $SUCCESS -ne 0 ]; then
    echo -e "[  BUILDING ]\t\terror: build failed" >&2
    popd > /dev/null
    exit 1
else
    echo -e "[  BUILDING ]\t\tSUCCESS"
fi


echo -e "[  INSTALLING ]\t\t$EXEC"
make install > /dev/null 2>&1
SUCCESS=$?
if [ $SUCCESS -ne 0 ]; then
    echo -e "[  INSTALLING ]\t\terror: install failed" >&2
    popd > /dev/null
    exit 1
else
    echo -e "[  INSTALLING ]\t\tSUCCESS"
fi

if [ "$COMMAND_ARG" == "test" ]; then
    echo -e "[  TESTING ]\t\tDRY RUN"
    echo -e "[  TESTING ]\t\t$CONFIG_FILE"
    $EXEC --config "$CONFIG_FILE" --dry-run --verbose
fi


if [ "$COMMAND_ARG" == "start" ]; then
    echo -e "[  TESTING ]\t\t$TEST_BIN --config \"$CONFIG_FILE\" --verbose"
    echo -e "[  TESTING ]\t\tREAL TEST"
    LOGFILE=$(mktemp /tmp/iboot-test-XXXX.log)
    echo " " > $LOGFILE
    $TEST_BIN --config "$CONFIG_FILE" --verbose > $LOGFILE 2>&1 &
    sleep 1
    chown $USER:USER $LOGFILE
    sleep 1
    chmod 777 $LOGFILE
    echo -e "[  TESTING ]\t\tRun: tail -f $LOGFILE"
fi

popd > /dev/null