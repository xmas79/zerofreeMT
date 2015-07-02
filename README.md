# zerofreeMT - zerofree MultiThread

This is the utility "zerofree" with mutithread support. Useful if you have a filesystem where an IO operation takes ages to complete (eg. a networked filesystem, no SSD or HDD). I used it to zerofree an ext4 filesystem runnning on top of s3backer, speeding up the zeroing process.

