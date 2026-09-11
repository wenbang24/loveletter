Push and compile
================

Commit your changes, then run:

    git push server main

The server remote is wenbang@hackclub.app:loveletter.git. Pushes to main
check out the source into ~/loveletter and compile ~/loveletter/server/out.
Other branches are stored without building. Builds run one at a time.
The build output appears during the push and is saved in ~/loveletter.git/build.log.
Git accepts the commit even if compilation fails; the old executable is retained.
To retry a failed build without a new commit:

    ssh wenbang@hackclub.app 'cd ~/loveletter.git && printf "retry retry refs/heads/main\n" | hooks/post-receive'

Successful builds also publish the executable and HTML to /srv/loveletter
and restart loveletter.service. Restarting clears active games and rooms.
The service starts automatically after reboot and runs as an unprivileged user.
To check status and logs:

    ssh wenbang@hackclub.app 'systemctl status loveletter --no-pager'
    ssh wenbang@hackclub.app 'journalctl -u loveletter -n 50 --no-pager'

The app listens on 127.0.0.1:8080. Nginx forwards port 80 to it, including
WebSockets. Nest's dashboard domain route must target port 80 for public HTTPS.
Direct public access (requires IPv6): http://[2a01:4f9:3081:399c::249]/
At setup time Nest's Domains page returned HTTP 500, so the HTTPS domain
route could not be configured. Add wenbang.hackclub.app targeting port 80
at https://hackclub.app/dashboard/domains when that page is working again.
Service config: /etc/systemd/system/loveletter.service.
Proxy config: /etc/nginx/sites-available/loveletter.
The checkout is managed by the hook; edit locally and push, not on the server.

Server prerequisites: git, g++, libasio-dev, flock, Bash.
The ignored local server/crow headers were copied to ~/loveletter-deps/crow.
If those headers change, copy them again separately.
The installed hook is ~/loveletter.git/hooks/post-receive. To update it:

    ssh wenbang@hackclub.app 'cat > ~/loveletter.git/hooks/post-receive && chmod +x ~/loveletter.git/hooks/post-receive' < deploy/post-receive
