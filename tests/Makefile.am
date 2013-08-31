#
# Makefile.am
#
# Author: Juan A. Suarez Romero <jasuarez@igalia.com>
#
# Copyright (C) 2013 Igalia S.L.

include $(top_srcdir)/gtester.mk

SUBDIRS =

if APPLE_TRAILERS_PLUGIN
SUBDIRS += apple-trailers
endif

if BLIPTV_PLUGIN
SUBDIRS += bliptv
endif

if TMDB_PLUGIN
SUBDIRS += tmdb
endif

if VIMEO_PLUGIN
SUBDIRS += vimeo
endif

if XML_FACTORY_PLUGIN
SUBDIRS += xml-factory
endif

DIST_SUBDIRS =    \
   apple-trailers \
   bliptv         \
   tmdb           \
   vimeo          \
   xml-factory

MAINTAINERCLEANFILES =  \
   *.in                 \
   *~

DISTCLEANFILES = $(MAINTAINERCLEANFILES)
