#!/bin/bash

NETCOREDBG=$1
TEST_NAME=$2
ASSEMB_PATH=$3
FILE_COMMANDS=$4

file_log=$(mktemp)
file_test="$TEST_NAME/log_test.txt"

$NETCOREDBG --interpreter=cli -ex "source $FILE_COMMANDS" -- dotnet "$ASSEMB_PATH" 2>&1 | tee "$file_log"

echo ""
echo "CLI LOG INTERPRET START"

skip_line()
{
    local line=$1

    # Replies

    # Note, we check only first 2 frames in backtraces, since in case of interop we can't count on other info.
    if [[ "$line" == "#0:"* ]] ||
       [[ "$line" == "#1:"* ]]
    then
        exit
    elif [[ "$line" =~ ^#[0-9]+:.* ]]
    then
        echo "1"
        exit
    fi

    # Ignore text sent by program itself, since we can't predict line of this text in log.
    if [[ "$line" == "<stdout_marker>"* ]]
    then
        echo "1"
        exit
    fi

    # Events

    if [[ -z "$line" ]] ||
       [[ "$line" == "library loaded:"* ]] ||
       [[ "$line" == "library unloaded:"* ]] ||
       [[ "$line" == "no symbols loaded, base address:"* ]] ||
       [[ "$line" == "symbols loaded, base address:"* ]] ||
       [[ "$line" == "thread created, id:"* ]] ||
       [[ "$line" == "thread exited, id:"* ]] ||
       [[ "$line" == "native thread created, id: "* ]] ||
       [[ "$line" == "managed thread created, id: "* ]] ||
       [[ "$line" == "native thread exited, id: "* ]] ||
       [[ "$line" == "managed thread exited, id: "* ]]
    then
        echo "1"
    else
        echo "0"
    fi
}

# Remove ^M ('\r') from each line in file, if have it ("sdb shell" usually add this one).
mv "$file_log" "$file_log"2
sed -e 's/\r//g' "$file_log"2 > "$file_log"
rm -f "$file_log"2

log_current_line=1
test_current_line=1
log_line_count=$(awk 'END{print NR}' "$file_log")
log_line_count=$((log_line_count+1))
test_line_count=$(awk 'END{print NR}' "$file_test")
test_line_count=$((test_line_count+1))

while [[ "$log_current_line" -lt "$log_line_count" ]] && [[ "$test_current_line" -lt "$test_line_count" ]]
do
    log_line=$(sed "$log_current_line"'q;d' "$file_log")
    log_current_line=$((log_current_line+1))

    test_line=$(sed "$test_current_line"'q;d' "$file_test")
    test_current_line=$((test_current_line+1))

    while [[ $(skip_line "$log_line") -eq "1" ]] && [[ "$log_current_line" -lt "$log_line_count" ]]
    do
        log_line=$(sed "$log_current_line"'q;d' "$file_log")
        log_current_line=$((log_current_line+1))
    done

    echo "TEST"
    echo "  log line   =""$log_line" | cat -v
    echo "  test regex =""$test_line" | cat -v

    # Note, "test_line" provide regex that have '\^' first symbol  instead of '^'.
    if [[ "$log_line" == "^exit" ]] && [[ "$test_line" == "\^exit" ]]
    then
        echo "OK"
        rm -f "$file_log"
        exit 0
    fi

    # Note, we don't include in test log file '^' (start string marker) and '$' (end string marker), but add them in comparation directly.
    if [[ "$log_line" =~ ^$test_line$ ]]
    then
        echo "OK"
    else
        echo "FAIL"
        rm -f "$file_log"
        exit 1
    fi

done

echo "FAIL"
rm -f "$file_log"
exit 1

