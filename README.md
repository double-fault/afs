# afs

afs - ashish's filesystem

ideas:

 - fuse
 - access over network (similar to sshfs, but code up your own server etc., use sftp only?)
 - later on, database as fs (mysqlfs types)
 - extend "everything is a file" to playing chess on lichess :clown:

named eventfd:

lets start with a named eventfd to get a hang of FUSE/fs stuff.

 - `fd = open("/afs/some_name")` creates a file in fs
 - `ioctl(fd, EVENTFD)` to initialize it as an eventfd, initial value 0.
 - `ioctl` or `fcntl` to set non blocking optionally.
 - `read(fd)` blocks until counter becomes non zero or returns with EAGAIN (non blocking). Returns 1 on success and counter is decremented by 1.
 - `write(fd)` adds the value to the counter. Used to initialize the initial value to something non zero also.
 - `poll`/`select`/`epoll` as expected.

Actual logic should just be a simple wrapper around actual eventfd.

Expose a file `/afs/some_name.stat` which exposes some stats about the eventfd (like which pid is holding it currently). Makes the lock observable externally - and possibly makes testing easier?

I mean this is mostly useless lol, but it does fill a gap in linux ipc ig. You want to share a semaphore b/w unrelated processes and in the processes wait using select/poll/epoll on the semaphore (and other stuff ofc). First option is posix named semaphores, but you cant use those in select/poll/epoll. eventfd you can, but sharing them b/w unrelated processes is awkward - you need to pass the open file _description_ using unix domain sockets (see SCM_RIGHTS in man 7 unix :pray:).
