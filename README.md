# sping - Stream Ping

Accept a list of target IPv4 addresses from stdin and output ping test result to stdout, with debug message directed to stderr.

Suitable for pipe based streaming input of test targets.

Support configuration of:

- maximum paralleling task count
- ignore first n packets as warm-up
- test n times and count into final result

## Output Format

| Field |                         Description                          |
| :---: | :----------------------------------------------------------: |
|  IP   |                          Target IP                           |
|  AVG  | Average RTT of valid ping tests (timeout-ed packet not considered) |
|  MIN  |                         Minimum RTT                          |
|  MAX  |        Maximum RTT (timeout-ed packet not considered)        |
|  ERR  |               Standard deviation of valid RTTs               |
| TOTAL |                 Number of total sent packets                 |
| LOST  |                 Number of timeout-ed packets                 |

## Input Format

One IP per line:

```
127.0.0.1
192.168.1.1
1.1.1.1
8.8.8.8
```

## Command line Interface

Refer to its help pages: `sping -h`

## Sample Usage

### Type IP into interactive Cli manually

Simply start the tool then it will wait for your input.

`CTRL + D` will generate an EOF which will notify the tool to exit

### Same as above, but only show result

Simply redirecting stderr to other place:

```sh
sping 2>/dev/null
```

### Test IPs from a file and only show result

```sh
cat some_file | sping 2>/dev/null
```

### Run test with 1024 parallel count

This will test 1024 IPs at the same time. (default is 16)

Will save significant amount of time when dealing with large set of targets.

```sh
cat some_file | sping -p 1024 2>/dev/null
```

