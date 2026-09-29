How to setup a development environment for uvroot?
=================================================

This document provides instructions for preparing
a system for developing uvroot and CARE.

Docker
------

The following command will attempt to build an image for
each supported distribution::

  make -C test check-test-docker.sh V=1

Vagrant
-------

**Note**: this requires installing the `vagrant-sshfs`_ plugin.

.. _vagrant-sshfs: https://github.com/dustymabe/vagrant-sshfs

The following command will initialize a virtual machine for
each supported distribution::

  for distro in alpine centos debian; do
    cd "test/vagrant/${distro}"
    vagrant up
  done
