#!/usr/bin/env python3

import sys, os

def usage():
    print('rivercheck.py source_config destination_config')

def getVal(listData):
    dictData = {}
    for data in listData:
        if data.startswith('#'):
            continue
        info = tuple(map(str.strip, data.split('=')))
        if len(info) != 2:
            continue
        dictData[info[0]] = info[1]
    return dictData

def checkVal(dictSrc,dictDst):
    dictDiff = {}
    for key in dictSrc:
        if key not in dictDst:
            dictDiff[key] = (dictSrc[key], '-')
            continue
        if dictSrc[key] != dictDst[key]:
            dictDiff[key] = (dictSrc[key], dictDst[key])
    return dictDiff

def printDict(dictData):
    for key, val in dictData.items() :
        print('{} : {} {}'.format(key, val[0], val[1]))


def saveDict(dictData):
    filename = os.getcwd()+'/config.diff.save'
    output = ''
    for key, val in dictData.items():
        output += '{}={}\n'.format(key,val[0])

    print(output)

    with open(filename,'w', encoding='utf-8') as wf :
        wf.write(output)


def main(src,dst,save=False):
    with open(src,'r',encoding='utf-8') as fsrc:
        listSrc = fsrc.read().split('\n')
        dictSrc = getVal(listSrc)
    with open(dst,'r',encoding='utf-8') as fdst:
        listDst = fdst.read().split('\n')
        dictDst = getVal(listDst)
    dictDiff = checkVal(dictSrc, dictDst)
    
    if save:
        saveDict(dictDiff)
    
    printDict(dictDiff)

if  __name__ == '__main__':
    argn = len(sys.argv)
    if argn not in [3,4] or not os.path.isfile(sys.argv[1]) or not os.path.isfile(sys.argv[2]):
        usage()
        sys.exit()
    if argn == 4 and sys.argv[3] == '-1':
        main(sys.argv[2], sys.argv[1])
    elif argn == 4 and sys.argv[3] == '1':
        main(sys.argv[1], sys.argv[2])
        print('-'*50)
        main(sys.argv[2], sys.argv[1])
    elif argn == 4 and sys.argv[3] == '11':
        main(sys.argv[1], sys.argv[2], True)
    else:
        main(sys.argv[1], sys.argv[2])


