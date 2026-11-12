.PHONY: build package deploy factory
build:
	sh build.sh
package: build
	python3 package.py
deploy: package
	python3 deploy.py
factory:
	python3 deploy.py --factory
