# Ardos

> A Realtime Distributed Object Server

*Based on [Astron](https://github.com/astron/Astron) (Modified BSD 3-Clause)*

Ardos is a highly-distributed networking suite designed to power large-scale MMO games.
Inspired by the in-house OTP (Online Theme Park) networking technology developed by Disney Interactive Studios, which
was used to power MMOs such as Toontown Online, Pirates of The Caribbean Online, Pixie Hollow Online, and The World of
Cars Online.

Ardos is first and foremost designed to be a first class citizen of the [Panda3D](https://www.panda3d.org/) game
engine (developed by Disney and CMU), which has built-in, production ready support for this server technology.
However, through tools such as [otp-gen](https://github.com/ksmit799/otp-gen), it's possible to use Ardos via other
game engines/languages.

It's highly recommended to view the Ardos [wiki](https://github.com/ksmit799/Ardos/wiki) for build instructions,
configuration examples, deployment strategies, networking topology, and help on determining whether Ardos is the right
fit for you.

Ardos is still early in its development, and as such you may encounter missing/incomplete features and erratic
behaviour.
Please see the [1.0.0 Milestone](https://github.com/ksmit799/Ardos/milestone/1) which tracks progress towards Ardos'
first major release.

## Requirements

### MongoDB Driver

NOTE: The MongoDB Driver is only required if you're building Ardos with `ARDOS_WANT_DB_SERVER`

See this [tutorial](https://www.mongodb.com/developer/products/mongodb/getting-started-mongodb-c/) for an installation
guide.

### Prometheus Metrics

NOTE: While Ardos requires Prometheus Metrics to build, it can be configured on/off via the config.
It's highly recommended to run Ardos with metrics enabled, as it allows insight into how a particular cluster is
operating/performing.

See this [tutorial](https://github.com/jupp0r/prometheus-cpp#via-cmake) for an installation guide.

## OTP Architecture Resources

> Helpful for understanding the internal architecture of Ardos and its usages.

### Video Lectures (Disney Interactive Studios)

- [DistributedObjects](http://www.youtube.com/watch?v=JsgCFVpXQtQ)
- [DistributedObjects and the OTP Server](http://www.youtube.com/watch?v=r_ZP9SInPcs)
- [OTP Server Internals](http://www.youtube.com/watch?v=SzybRdxjYoA)
- [(GDC Online) MMO 101 - Building Disney's Server System](https://www.gdcvault.com/play/1013776/MMO-101-Building-Disney-s)

### Presentation Slides

- [MMO 101 - Building Disney's Server](http://twvideo01.ubm-us.net/o1/vault/gdconline10/slides/11516-MMO_101_Building_Disneys_Sever.pdf)

### Other Documentation

- [Building a MMG for the Millions - Disney's Toontown](http://dl.acm.org/citation.cfm?id=950566.950589)
