# mmhd: micro http server for makdown

micro http server for makdown

## Dependencies

libmicrohttpd library.

* [libmicrohttpd](http://www.gnu.org/software/libmicrohttpd)

sundown library.

* [sundown](https://github.com/kjdev/sundown)

## Build

required.

* [cmake](http://www.cmake.org)
* [libmicrohttpd](http://www.gnu.org/software/libmicrohttpd)

```
% cmake .
% make
% make install
```

## Application

 command | description
 ------- | -----------
 mmhd    | micro http server for markdown

### Application options

 option          | description               | default
 ------          | -----------               | -------
 -p, --port      | server bind port          | 8888
 -r, --rootdir   | document root directory   | .
 -d, --directory | directory index file name | index.md
 -s, --style     | style file                |
 -t, --toc       | enable table of contents  |
 -D, --daemonize | daemon command            |
 -P, --pidfile   | daemon pid file path      | /tmp/mmhd.pid

## Run

default bind port is 8888 and document root directory is '.'.

```
% mmhd [-p 8888] [-r .]
```

client access.

```
% curl http://localhost:8888/test.md
```

`.md`, `.markdown` file is converted.

### Run options

set document root directory (/path/to/name).

```
% mmhd -r /path/to/name
```

set directory index (index.html).

```
% mmhd -d index.html
```

set style file (/path/to/style.html).

```
% mmhd -s /path/to/style.html
```

/path/to/style.html:

```
<!DOCTYPE html>
<html>
<head>
<meta http-equiv="Content-Type" content="text/html; charset=UTF-8" />
<title>Markdown</title>
</head>
<body>
</body>
</html>
```

output after being converted into html from markdown in front of `</body>`.

the other option confirm `--help`.
