set tabstop=4 shiftwidth=4 expandtab

" configure make system
set autowrite
map <F8> :make
map <F4> :cn

if filereadable(glob(".vimrc.local"))
    source .vimrc.local
endif
