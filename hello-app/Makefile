PYTHON ?= python3
VERSION ?= 1.0.2

.PHONY: help package install activate factory

help:
	@echo 'make package VERSION=1.0.2   Build the versioned QML bundle'
	@echo 'make install VERSION=1.0.2   Verify and install through ADB'
	@echo 'make activate VERSION=1.0.1  Switch to an installed release'
	@echo 'make factory                Restore the factory recorder UI'
	@echo 'This demo uses ADB and SHA-256 integrity checks; bundles are unsigned.'

package:
	$(PYTHON) package.py $(VERSION)

install: package
	$(PYTHON) install-adb.py $(VERSION)

activate:
	$(PYTHON) install-adb.py --activate $(VERSION)

factory:
	$(PYTHON) install-adb.py --factory
