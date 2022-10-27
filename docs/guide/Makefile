
GUIDE:=netcoredbg-guide
PROGRAM:=$(GUIDE).pdf
SRC:=$(GUIDE).tex

all: $(SRC)
	pdflatex $(SRC)
	makeindex -p 1 ${GUIDE}.idx
	makeindex -p 1 notion.idx
	pdflatex $(SRC)

clean:
	@rm -rf *.blg *.bib *.aux *.bcf *.idx *.ind *.bbl *.dvi *.out *.svg *.pdf *.ilg *.log *.toc
